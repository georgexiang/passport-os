// main/apps/away/away_core.c —— 见 away_core.h。纯 C + cJSON,宿主机可直接编译测试。
#include "away_core.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

static void take(char *dst, size_t cap, const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    const char *s = cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
    size_t n = strlen(s);
    if (n >= cap) {   // 按 UTF-8 字符边界截断
        n = cap - 1;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    }
    memcpy(dst, s, n);
    dst[n] = '\0';
}

static double num(const cJSON *o, const char *key, double fallback) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

static bool contains(const char *hay, size_t len, const char *needle) {
    const size_t n = strlen(needle);
    for (size_t i = 0; i + n <= len; i++)
        if (memcmp(hay + i, needle, n) == 0) return true;
    return false;
}

static away_msg_t parse_unread(const cJSON *root, uint64_t now_ms, away_board_t *st) {
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
    const double part_d = num(root, "part", 0), parts_d = num(root, "parts", 1), seq_d = num(root, "seq", 0);
    if ((items && !cJSON_IsArray(items)) || part_d < 0 || parts_d < 1 || parts_d > AWAY_MAX_PARTS ||
        part_d >= parts_d || seq_d < 0 || seq_d > 4e9)
        return AWAY_MSG_BAD;
    const uint8_t part = (uint8_t)part_d, parts = (uint8_t)parts_d;
    const uint32_t seq = (uint32_t)seq_d;
    if (part == 0) {
        memset(st, 0, sizeof(*st));
        st->seq = seq;
        const double total = num(root, "total", 0), checked = num(root, "checked", 0);
        st->total = total > 0 && total < 65535 ? (uint16_t)total : 0;
        st->checked = checked > 0 && checked < 4.2e9 ? (uint32_t)checked : 0;
    }
    if (part != st->next_part || seq != st->seq) {
        st->next_part = 0xFF;
        return AWAY_MSG_DROPPED;
    }
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, items) {
        if (st->count >= AWAY_MAX_ITEMS) break;
        if (!cJSON_IsObject(it)) continue;
        away_item_t *g = &st->items[st->count++];
        take(g->name, sizeof(g->name), it, "n");
        take(g->source, sizeof(g->source), it, "s");
        const double c = num(it, "c", 0);
        g->count = c > 0 && c < 65535 ? (uint16_t)c : 0;
        g->muted = num(it, "m", 0) != 0;
    }
    st->next_part = (uint8_t)(part + 1);
    if (st->next_part < parts) return AWAY_MSG_PENDING;
    if (st->total < st->count) st->total = st->count;
    st->received_ms = now_ms ? now_ms : 1;
    st->next_part = 0xFF;
    return AWAY_MSG_COMPLETE;
}

away_msg_t away_parse(const char *json, size_t len, uint64_t now_ms,
                      away_board_t *staging, bool *away, uint32_t *since) {
    if (!json || len == 0) return AWAY_MSG_NONE;
    // 通道上每秒还有 pc.status / mihomo.state,先便宜地筛掉
    if (!contains(json, len, "\"away.")) return AWAY_MSG_NONE;
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return AWAY_MSG_BAD;
    away_msg_t res = AWAY_MSG_NONE;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (cJSON_IsString(type) && strcmp(type->valuestring, "away.state") == 0) {
        const cJSON *a = cJSON_GetObjectItemCaseSensitive(root, "away");
        if (!cJSON_IsBool(a)) {
            res = AWAY_MSG_BAD;
        } else {
            if (away) *away = cJSON_IsTrue(a);
            const double s = num(root, "since", 0);
            if (since) *since = s > 0 && s < 4.2e9 ? (uint32_t)s : 0;
            res = AWAY_MSG_STATE;
        }
    } else if (cJSON_IsString(type) && strcmp(type->valuestring, "away.unread") == 0) {
        res = staging ? parse_unread(root, now_ms, staging) : AWAY_MSG_BAD;
    }
    cJSON_Delete(root);
    return res;
}

uint32_t away_unread_total(const away_board_t *b) {
    uint32_t n = 0;
    for (uint8_t i = 0; i < b->count; i++) n += b->items[i].count;
    return n;
}

static const away_item_t *find(const away_board_t *b, const away_item_t *x) {
    for (uint8_t i = 0; i < b->count; i++)
        if (strcmp(b->items[i].name, x->name) == 0 && strcmp(b->items[i].source, x->source) == 0)
            return &b->items[i];
    return NULL;
}

bool away_should_alert(const away_board_t *prev, const away_board_t *next) {
    for (uint8_t i = 0; i < next->count; i++) {
        const away_item_t *n = &next->items[i];
        if (n->count == 0) continue;
        const away_item_t *p = prev ? find(prev, n) : NULL;
        if (!p || n->count > p->count) return true;
    }
    return false;
}

void away_format_duration(uint32_t s, char *out, size_t cap) {
    if (s < 60) snprintf(out, cap, "刚刚");
    else if (s < 3600) snprintf(out, cap, "%u 分钟", (unsigned)(s / 60));
    else snprintf(out, cap, "%u 小时 %u 分", (unsigned)(s / 3600), (unsigned)(s % 3600 / 60));
}
