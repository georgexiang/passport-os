#include "apps/orca/orca_core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool parse(const char *json, uint64_t now, orca_board_t *b) {
    return orca_parse_snapshot(json, strlen(json), now, b);
}

int main(void) {
    orca_board_t b, prev;
    memset(&prev, 0, sizeof prev);

    // Not our message type / malformed.
    assert(!parse("{\"type\":\"agent.status\"}", 1, &b));
    assert(!parse("{oops", 1, &b));
    assert(!parse("{\"type\":\"orca.snapshot\",\"agents\":5}", 1, &b));

    // Full snapshot.
    const char *snap =
        "{\"type\":\"orca.snapshot\",\"source\":\"arm-ubuntu-westus2\",\"total\":12,\"agents\":["
        "{\"t\":\"claude\",\"s\":\"waiting\",\"r\":\"qm-product\",\"b\":\"main\",\"k\":\"允许设置 SANDBOX\",\"tool\":\"Bash\",\"age\":95},"
        "{\"t\":\"copilot\",\"s\":\"working\",\"r\":\"qm-product\",\"b\":\"main\",\"k\":\"继续升级\",\"tool\":\"bash\",\"age\":30},"
        "{\"t\":\"codex\",\"s\":\"done\",\"r\":\"opencode\",\"age\":7200},"
        "{\"t\":\"pi\",\"s\":\"weird\"}]}";
    assert(parse(snap, 1000, &b));
    assert(b.count == 4 && b.total == 12 && strcmp(b.source, "arm-ubuntu-westus2") == 0);
    assert(b.agents[0].state == ORCA_ST_WAITING && strcmp(b.agents[0].tool, "Bash") == 0);
    assert(strcmp(b.agents[0].task, "允许设置 SANDBOX") == 0 && b.agents[0].age_s == 95);
    assert(b.agents[2].state == ORCA_ST_DONE && b.agents[2].branch[0] == '\0');
    assert(b.agents[3].state == ORCA_ST_UNKNOWN);
    assert(orca_attention_count(&b) == 1 && orca_count_state(&b, ORCA_ST_WORKING) == 1);

    // Freshness.
    assert(orca_board_fresh(&b, 1000 + ORCA_STALE_MS) && !orca_board_fresh(&b, 1001 + ORCA_STALE_MS));
    assert(!orca_board_fresh(&prev, 5));

    // Alerts: first time a waiting agent appears; not again while it stays; again for a new one.
    assert(orca_should_alert(&prev, &b));
    orca_board_t same = b;
    assert(!orca_should_alert(&b, &same));
    orca_board_t more = b;
    more.agents[1].state = ORCA_ST_BLOCKED;
    assert(orca_should_alert(&b, &more));
    orca_board_t calm = b;
    calm.agents[0].state = ORCA_ST_WORKING;
    assert(!orca_should_alert(&b, &calm));
    assert(orca_should_alert(&calm, &b));   // waiting again after it cleared

    // More than ORCA_MAX_AGENTS entries are capped; total falls back to count if absent.
    char big[2048] = "{\"type\":\"orca.snapshot\",\"agents\":[";
    for (int i = 0; i < ORCA_MAX_AGENTS + 4; i++) strcat(big, i ? ",{\"t\":\"x\",\"s\":\"done\"}" : "{\"t\":\"x\",\"s\":\"done\"}");
    strcat(big, "]}");
    assert(parse(big, 7, &b) && b.count == ORCA_MAX_AGENTS && b.total == ORCA_MAX_AGENTS);

    // Multi-part snapshots: parts must arrive in order with the same seq.
    orca_board_t st; memset(&st, 0, sizeof st);
#define PART(seq, part, parts, n) \
    "{\"type\":\"orca.snapshot\",\"source\":\"srv\",\"total\":13,\"seq\":" #seq ",\"part\":" #part \
    ",\"parts\":" #parts ",\"agents\":[" n "]}"
#define A "{\"t\":\"a\",\"s\":\"done\"}"
    const char *p0 = PART(5, 0, 3, A "," A "," A "," A "," A);
    const char *p1 = PART(5, 1, 3, A "," A "," A "," A "," A);
    const char *p2 = PART(5, 2, 3, A "," A "," A);
    assert(orca_parse_part(p0, strlen(p0), 100, &st) == ORCA_PART_PENDING && st.count == 5);
    assert(orca_parse_part(p1, strlen(p1), 100, &st) == ORCA_PART_PENDING && st.count == 10);
    assert(orca_parse_part(p2, strlen(p2), 100, &st) == ORCA_PART_COMPLETE);
    assert(st.count == 13 && st.total == 13 && st.received_ms == 100 && strcmp(st.source, "srv") == 0);
    // Missing middle part: dropped, and later parts of that seq are ignored.
    memset(&st, 0, sizeof st);
    const char *q0 = PART(6, 0, 3, A), *q2 = PART(6, 2, 3, A);
    assert(orca_parse_part(q0, strlen(q0), 1, &st) == ORCA_PART_PENDING);
    assert(orca_parse_part(q2, strlen(q2), 1, &st) == ORCA_PART_DROPPED && st.received_ms == 0);
    const char *q1 = PART(6, 1, 3, A);
    assert(orca_parse_part(q1, strlen(q1), 1, &st) == ORCA_PART_DROPPED);
    // A new seq starting at part 0 recovers.
    assert(orca_parse_part(p0, strlen(p0), 2, &st) == ORCA_PART_PENDING);
    // Mixing seqs mid-snapshot drops it.
    const char *r1 = PART(7, 1, 3, A);
    assert(orca_parse_part(r1, strlen(r1), 2, &st) == ORCA_PART_DROPPED);
    // Bad part numbers and other channel messages.
    const char *bad = PART(8, 3, 3, A);
    assert(orca_parse_part(bad, strlen(bad), 3, &st) == ORCA_PART_BAD);
    const char *other = "{\"type\":\"mihomo.state\",\"running\":true}";
    assert(orca_parse_part(other, strlen(other), 3, &st) == ORCA_PART_NOT_SNAPSHOT);
    // Agents beyond ORCA_MAX_AGENTS across parts are capped, not overflowed.
    memset(&st, 0, sizeof st);
    for (int k = 0; k < 5; k++) {
        char pk[512];
        snprintf(pk, sizeof pk, "{\"type\":\"orca.snapshot\",\"seq\":9,\"part\":%d,\"parts\":5,\"agents\":["
                 A "," A "," A "," A "," A "," A "]}", k);
        orca_part_result_t r = orca_parse_part(pk, strlen(pk), 4, &st);
        assert(r == (k == 4 ? ORCA_PART_COMPLETE : ORCA_PART_PENDING));
    }
    assert(st.count == ORCA_MAX_AGENTS && st.total == ORCA_MAX_AGENTS);
#undef A
#undef PART

    // UTF-8 truncation never splits a character (task holds 63 bytes: 21 full 3-byte chars).
    char longtask[512];
    snprintf(longtask, sizeof longtask, "{\"type\":\"orca.snapshot\",\"agents\":[{\"t\":\"c\",\"s\":\"done\",\"k\":\"%s\"}]}",
             "一二三四五六七八九十一二三四五六七八九十一二三四五六七八九十");
    assert(parse(longtask, 9, &b) && strlen(b.agents[0].task) == 63);

    // Age formatting.
    char a[16];
    orca_format_age(5, a, sizeof a); assert(strcmp(a, "5秒") == 0);
    orca_format_age(125, a, sizeof a); assert(strcmp(a, "2分钟") == 0);
    orca_format_age(7200, a, sizeof a); assert(strcmp(a, "2小时") == 0);
    orca_format_age(200000, a, sizeof a); assert(strcmp(a, "2天") == 0);

    puts("orca_core: parse, freshness, alerts, caps, utf-8 truncation, age PASS");
    return 0;
}
