#!/usr/bin/env python3
"""Send a test `agent.status` line to the Passport voice app over its UDP channel.

The device only accepts EVENT packets from the peer it learned via a beacon, so this
script also broadcasts the beacon itself. Quit Passport Voice on the Mac first:
both need UDP 33333.

    python3 tools/send_agent_status.py attention "● 等你：claude qm-product"
    python3 tools/send_agent_status.py running   "● 工作中 2 · copilot…"
    python3 tools/send_agent_status.py ready     "没有 agent"

The status is resent every 5 s for --seconds (default 30), like the Mac relay will.
After it stops the device hides the line after 15 s.
"""
import argparse
import json
import socket
import time

PORT = 33333
BEACON = b"\x03" + json.dumps({"v": 1, "role": "pc", "port": PORT}).encode()

ap = argparse.ArgumentParser()
ap.add_argument("state", choices=["ready", "thinking", "running", "error", "done", "attention"])
ap.add_argument("message")
ap.add_argument("--seconds", type=float, default=30)
args = ap.parse_args()

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
s.bind(("0.0.0.0", PORT))
s.settimeout(0.2)

device = None
status = b"\x02" + json.dumps({"type": "agent.status", "state": args.state,
                               "message": args.message}, ensure_ascii=False).encode() + b"\n"
ready = b"\x02" + json.dumps({"type": "pc.status", "audioReady": True, "hotkeyReady": True}).encode() + b"\n"
end = time.time() + args.seconds
last_beacon = last_status = 0.0
while time.time() < end:
    now = time.time()
    if now - last_beacon >= 1:
        s.sendto(BEACON, ("255.255.255.255", PORT))
        if device:
            s.sendto(ready, device)
        last_beacon = now
    if device and now - last_status >= 5:
        s.sendto(status, device)
        print(f"→ {device[0]} agent.status {args.state}: {args.message}")
        last_status = now
    try:
        data, addr = s.recvfrom(2048)
    except socket.timeout:
        continue
    if data[:1] != b"\x03" and device != addr:
        device = addr
        print(f"device connected: {addr[0]}")
print("done (device hides the line ~15 s after the last update)")
