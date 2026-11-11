#include "apps/away/away_core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static away_msg_t parse(const char *json, uint64_t now, away_board_t *st, bool *away, uint32_t *since) {
    return away_parse(json, strlen(json), now, st, away, since);
}

int main(void) {
    away_board_t st, prev;
    memset(&st, 0, sizeof st);
    memset(&prev, 0, sizeof prev);
    bool away = false;
    uint32_t since = 0;

    // Other traffic on the channel is ignored.
    assert(parse("{\"type\":\"pc.status\",\"audioReady\":true}", 1, &st, &away, &since) == AWAY_MSG_NONE);
    assert(parse("{\"type\":\"orca.snapshot\"}", 1, &st, &away, &since) == AWAY_MSG_NONE);
    assert(parse("{\"type\":\"away.state\",\"away\":", 1, &st, &away, &since) == AWAY_MSG_BAD);
    assert(parse("{\"type\":\"away.state\",\"away\":1}", 1, &st, &away, &since) == AWAY_MSG_BAD);

    // State.
    assert(parse("{\"type\":\"away.state\",\"away\":true,\"since\":1731230000}", 1, &st, &away, &since) == AWAY_MSG_STATE);
    assert(away && since == 1731230000u);
    assert(parse("{\"type\":\"away.state\",\"away\":false}", 1, &st, &away, &since) == AWAY_MSG_STATE);
    assert(!away && since == 0);

    // Single-part unread snapshot.
    const char *one =
        "{\"type\":\"away.unread\",\"seq\":1,\"total\":2,\"checked\":1731230300,\"items\":["
        "{\"n\":\"项目沟通群\",\"c\":2,\"m\":1,\"s\":\"飞书\"},"
        "{\"n\":\"值班专项处理群\",\"c\":1,\"m\":0,\"s\":\"小米\"}]}";
    assert(parse(one, 1000, &st, &away, &since) == AWAY_MSG_COMPLETE);
    assert(st.count == 2 && st.total == 2 && st.checked == 1731230300u && st.received_ms == 1000);
    assert(st.items[0].muted && st.items[0].count == 2 && strcmp(st.items[0].source, "飞书") == 0);
    assert(!st.items[1].muted && strcmp(st.items[1].name, "值班专项处理群") == 0);
    assert(away_unread_total(&st) == 3);

    // Alerts: first sight, then not again for the same counts, again when a count grows or a group appears.
    assert(away_should_alert(&prev, &st));
    prev = st;
    assert(parse(one, 2000, &st, &away, &since) == AWAY_MSG_COMPLETE);
    assert(!away_should_alert(&prev, &st));
    const char *more =
        "{\"type\":\"away.unread\",\"seq\":2,\"items\":[{\"n\":\"项目沟通群\",\"c\":3,\"m\":1,\"s\":\"飞书\"}]}";
    assert(parse(more, 3000, &st, &away, &since) == AWAY_MSG_COMPLETE);
    assert(away_should_alert(&prev, &st));
    const char *fewer =
        "{\"type\":\"away.unread\",\"seq\":3,\"items\":[{\"n\":\"项目沟通群\",\"c\":1,\"m\":1,\"s\":\"飞书\"}]}";
    assert(parse(fewer, 3000, &st, &away, &since) == AWAY_MSG_COMPLETE);
    assert(!away_should_alert(&prev, &st));
    // Same name from another account counts as new.
    const char *other =
        "{\"type\":\"away.unread\",\"seq\":4,\"items\":[{\"n\":\"项目沟通群\",\"c\":1,\"m\":1,\"s\":\"公司\"}]}";
    assert(parse(other, 3000, &st, &away, &since) == AWAY_MSG_COMPLETE);
    assert(away_should_alert(&prev, &st));
    // Empty snapshot: nothing to alert.
    assert(parse("{\"type\":\"away.unread\",\"seq\":5,\"items\":[]}", 4000, &st, &away, &since) == AWAY_MSG_COMPLETE);
    assert(st.count == 0 && !away_should_alert(&prev, &st));

    // Multi-part: in order completes; out of order drops.
    assert(parse("{\"type\":\"away.unread\",\"seq\":9,\"part\":0,\"parts\":2,\"total\":2,\"items\":[{\"n\":\"A\",\"c\":1}]}",
                 5000, &st, &away, &since) == AWAY_MSG_PENDING);
    assert(parse("{\"type\":\"away.unread\",\"seq\":9,\"part\":1,\"parts\":2,\"items\":[{\"n\":\"B\",\"c\":4}]}",
                 5000, &st, &away, &since) == AWAY_MSG_COMPLETE);
    assert(st.count == 2 && away_unread_total(&st) == 5);
    assert(parse("{\"type\":\"away.unread\",\"seq\":10,\"part\":1,\"parts\":2,\"items\":[]}",
                 5000, &st, &away, &since) == AWAY_MSG_DROPPED);
    assert(parse("{\"type\":\"away.unread\",\"seq\":11,\"part\":0,\"parts\":2,\"items\":[]}",
                 5000, &st, &away, &since) == AWAY_MSG_PENDING);
    assert(parse("{\"type\":\"away.unread\",\"seq\":12,\"part\":1,\"parts\":2,\"items\":[]}",
                 5000, &st, &away, &since) == AWAY_MSG_DROPPED);
    assert(parse("{\"type\":\"away.unread\",\"seq\":1,\"part\":3,\"parts\":2}", 1, &st, &away, &since) == AWAY_MSG_BAD);

    // Long names are cut on a UTF-8 boundary.
    char longj[512];
    snprintf(longj, sizeof longj, "{\"type\":\"away.unread\",\"items\":[{\"n\":\"%s\",\"c\":1}]}",
             "一二三四五六七八九十一二三四五六七八九十一二三四五六七八九十");
    assert(parse(longj, 1, &st, &away, &since) == AWAY_MSG_COMPLETE);
    assert(strlen(st.items[0].name) < sizeof st.items[0].name && strlen(st.items[0].name) % 3 == 0);

    char d[32];
    away_format_duration(30, d, sizeof d); assert(strcmp(d, "刚刚") == 0);
    away_format_duration(23 * 60, d, sizeof d); assert(strcmp(d, "23 分钟") == 0);
    away_format_duration(3900, d, sizeof d); assert(strcmp(d, "1 小时 5 分") == 0);

    puts("away core: ok");
    return 0;
}
