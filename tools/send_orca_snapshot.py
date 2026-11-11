#!/usr/bin/env python3
"""Feed the Passport 「Orca 看板」 app with agent snapshots over UDP 33333.

Test tool for the device side; the real feed will come from the Mac app later.
Quit Passport Voice on the Mac first: both need UDP 33333.

    # Demo data, cycling through working -> needs you -> idle
    python3 tools/send_orca_snapshot.py demo

    # Real data from Orca via the orca CLI (local, or a paired server)
    python3 tools/send_orca_snapshot.py live
    python3 tools/send_orca_snapshot.py live --environment arm-ubuntu-westus2

Protocol: see main/apps/orca/orca_core.h.
"""
import argparse
import json
import socket
import subprocess
import time

PORT = 33333
BEACON = b"\x03" + json.dumps({"v": 1, "role": "pc", "port": PORT}).encode()
RANK = {"waiting": 0, "blocked": 1, "working": 2, "done": 3}
MAX_AGENTS = 20     # device keeps at most this many (ORCA_MAX_AGENTS)
MAX_PARTS = 8       # ORCA_MAX_PARTS
PACKET_MAX = 512    # one UDP EVENT, including the 0x02 type byte and trailing '\n'
_seq = 0


def clip(s, n):
    s = " ".join((s or "").split())
    return s if len(s) <= n else s[: n - 1] + "…"


def row(a):
    """Compact one agent. Only active agents carry a task: that's what the detail
    page is for; finished ones just need their row."""
    active = a.get("s") in ("working", "waiting", "blocked")
    r = {"t": clip(a["t"], 15), "s": a["s"], "r": clip(a.get("r", ""), 20)}
    if a.get("b") and a["b"] not in ("main", "master"):
        r["b"] = clip(a["b"], 20)
    if active and a.get("k"):
        r["k"] = clip(a["k"], 20)
    if active and a.get("tool"):
        r["tool"] = clip(a["tool"], 15)
    if a.get("age"):
        r["age"] = a["age"]
    return r


def packets(source, total, agents):
    """Split one snapshot into as many ≤512-byte EVENT lines as needed (in order)."""
    global _seq
    _seq += 1
    rows = [row(a) for a in agents[:MAX_AGENTS]]

    def line(part, parts, chunk):
        body = json.dumps({"type": "orca.snapshot", "source": clip(source, 20), "total": total,
                           "seq": _seq, "part": part, "parts": parts, "agents": chunk},
                          ensure_ascii=False, separators=(",", ":"))
        return b"\x02" + body.encode() + b"\n"

    # Greedy fill; parts count isn't known yet, so measure with a 1-digit placeholder.
    chunks, cur = [], []
    for r in rows:
        if cur and len(line(0, 9, cur + [r])) > PACKET_MAX:
            chunks.append(cur)
            cur = []
        cur.append(r)
    chunks.append(cur)
    chunks = chunks[:MAX_PARTS]
    out = [line(i, len(chunks), c) for i, c in enumerate(chunks)]
    assert all(len(p) <= PACKET_MAX for p in out), "a single agent row exceeds 512 bytes"
    return out


def demo_snapshot(step):
    phase = (step // 4) % 3
    agents = [
        {"t": "copilot", "s": "working", "r": "qm-product", "b": "main", "k": "继续指挥 claude 进行 agentops 升级", "tool": "bash", "age": 30 + step * 5},
        {"t": "claude", "s": "waiting" if phase == 1 else "working", "r": "qm-product", "b": "main",
         "k": "允许仅在演练环境设置 SANDBOX_BACKEND=local", "tool": "Bash", "age": 95},
        {"t": "codex", "s": "done", "r": "opencode", "b": "", "k": "", "age": 7200},
        {"t": "pi", "s": "done", "r": "azure-operation", "b": "", "k": "", "age": 86400 * 3},
    ]
    if phase == 2:
        for a in agents:
            a["s"] = "done"
    agents.sort(key=lambda a: RANK.get(a["s"], 9))
    return "demo", 12, agents


def live_snapshot(environment):
    cmd = ["orca", "worktree", "ps", "--json"] + (["--environment", environment] if environment else [])
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=30).stdout
    worktrees = json.loads(out)["result"]["worktrees"]
    now = time.time()
    agents = []
    for w in worktrees:
        if w.get("isArchived"):
            continue
        branch = (w.get("branch") or "").replace("refs/heads/", "")
        for a in w.get("agents") or []:
            started = (a.get("stateStartedAt") or 0) / 1000
            agents.append({"t": a.get("agentType") or "agent", "s": a.get("state") or "idle",
                           "r": clip(w.get("repo") or "?", 20), "b": clip(branch, 20),
                           "k": a.get("taskTitle") or a.get("displayName") or a.get("prompt") or "",
                           "tool": clip(a.get("toolName") or "", 12),
                           "age": int(now - started) if started else 0})
    agents.sort(key=lambda a: (RANK.get(a["s"], 9), a["age"]))
    return environment or "本机", len(agents), agents


ap = argparse.ArgumentParser()
ap.add_argument("mode", choices=["demo", "live"])
ap.add_argument("--environment", default="")
ap.add_argument("--seconds", type=float, default=600)
args = ap.parse_args()

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
s.bind(("0.0.0.0", PORT))
s.settimeout(0.2)

device = None
end = time.time() + args.seconds
last_beacon = last_snap = 0.0
step = 0
while time.time() < end:
    now = time.time()
    if now - last_beacon >= 1:
        s.sendto(BEACON, ("255.255.255.255", PORT))
        last_beacon = now
    if device and now - last_snap >= 5:
        try:
            source, total, agents = demo_snapshot(step) if args.mode == "demo" else live_snapshot(args.environment)
        except Exception as e:  # keep feeding; the device shows stale after 15 s
            print("snapshot failed:", e)
            last_snap = now
            continue
        parts = packets(source, total, agents)
        for p in parts:
            s.sendto(p, device)
            time.sleep(0.02)   # keep parts in order and give the device's RX task room
        sizes = "+".join(str(len(p)) for p in parts)
        shown = min(len(agents), MAX_AGENTS)
        print(f"→ {device[0]} {len(parts)} packets ({sizes}B) {source}: {shown}/{total} agents")
        last_snap = now
        step += 1
    try:
        data, addr = s.recvfrom(2048)
    except socket.timeout:
        continue
    if data[:1] != b"\x03" and device != addr:
        device = addr
        print(f"device connected: {addr[0]}")
        last_snap = 0
print("done")
