// main/apps/orca/orca_core.c —— 见 orca_core.h。纯 C + cJSON,宿主机可直接编译测试。
#include "orca_core.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

static void take(char *dst, size_t cap, const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    const char *s = cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
    // 按 UTF-8 字符边界截断,避免把汉字切成半个导致乱码。
    size_t n = strlen(s);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    }
    memcpy(dst, s, n);
    dst[n] = '\0';
}

static uint8_t parse_state(const char *s) {
    if (strcmp(s, "working") == 0) return ORCA_ST_WORKING;
    if (strcmp(s, "waiting") == 0) return ORCA_ST_WAITING;
    if (strcmp(s, "blocked") == 0) return ORCA_ST_BLOCKED;
    if (strcmp(s, "done") == 0) return ORCA_ST_DONE;
    if (strcmp(s, "idle") == 0) return ORCA_ST_IDLE;
    return ORCA_ST_UNKNOWN;
}

static bool contains(const char *hay, size_t len, const char *needle) {
    const size_t n = strlen(needle);
    for (size_t i = 0; i + n <= len; i++)
        if (memcmp(hay + i, needle, n) == 0) return true;
    return false;
}

static double num(const cJSON *o, const char *key, double fallback) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

orca_part_result_t orca_parse_part(const char *json, size_t len, uint64_t now_ms,
                                   orca_board_t *st) {
    if (!json || !st || len == 0) return ORCA_PART_NOT_SNAPSHOT;
    // Cheap pre-check: the channel also carries pc.status / mihomo.state every second.
    if (!contains(json, len, "orca.snapshot")) return ORCA_PART_NOT_SNAPSHOT;
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return ORCA_PART_BAD;
    orca_part_result_t res = ORCA_PART_BAD;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *agents = cJSON_GetObjectItemCaseSensitive(root, "agents");
    const double part_d = num(root, "part", 0), parts_d = num(root, "parts", 1);
    const double seq_d = num(root, "seq", 0);
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "orca.snapshot") != 0) {
        res = ORCA_PART_NOT_SNAPSHOT;
    } else if ((agents != NULL && !cJSON_IsArray(agents)) || part_d < 0 || parts_d < 1 ||
               parts_d > ORCA_MAX_PARTS || part_d >= parts_d || seq_d < 0 || seq_d > 4e9) {
        res = ORCA_PART_BAD;
    } else {
        const uint8_t part = (uint8_t)part_d, parts = (uint8_t)parts_d;
        const uint32_t seq = (uint32_t)seq_d;
        if (part == 0) {
            memset(st, 0, sizeof(*st));
            st->seq = seq;
            take(st->source, sizeof(st->source), root, "source");
            const double total = num(root, "total", -1);
            st->total = total >= 0 && total < 65535 ? (uint16_t)total : 0;
        }
        if (part != st->next_part || seq != st->seq) {
            st->next_part = 0xFF;   // 乱序或缺包:等下一份从 part 0 开始
            res = ORCA_PART_DROPPED;
        } else {
            const cJSON *a = NULL;
            cJSON_ArrayForEach(a, agents) {
                if (st->count >= ORCA_MAX_AGENTS) break;
                if (!cJSON_IsObject(a)) continue;
                orca_agent_t *g = &st->agents[st->count++];
                take(g->type, sizeof(g->type), a, "t");
                take(g->repo, sizeof(g->repo), a, "r");
                take(g->branch, sizeof(g->branch), a, "b");
                take(g->task, sizeof(g->task), a, "k");
                take(g->tool, sizeof(g->tool), a, "tool");
                char s[16];
                take(s, sizeof(s), a, "s");
                g->state = parse_state(s);
                const double age = num(a, "age", 0);
                g->age_s = age > 0 && age < 4e9 ? (uint32_t)age : 0;
            }
            st->next_part = (uint8_t)(part + 1);
            if (st->next_part == parts) {
                if (st->total < st->count) st->total = st->count;
                st->received_ms = now_ms ? now_ms : 1;
                st->next_part = 0xFF;
                res = ORCA_PART_COMPLETE;
            } else {
                res = ORCA_PART_PENDING;
            }
        }
    }
    cJSON_Delete(root);
    return res;
}

bool orca_parse_snapshot(const char *json, size_t len, uint64_t now_ms, orca_board_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    return orca_parse_part(json, len, now_ms, out) == ORCA_PART_COMPLETE;
}

bool orca_board_fresh(const orca_board_t *b, uint64_t now_ms) {
    return b->received_ms != 0 && now_ms - b->received_ms <= ORCA_STALE_MS;
}

uint8_t orca_count_state(const orca_board_t *b, orca_state_t st) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < b->count; i++) n += b->agents[i].state == st;
    return n;
}

uint8_t orca_attention_count(const orca_board_t *b) {
    return orca_count_state(b, ORCA_ST_WAITING) + orca_count_state(b, ORCA_ST_BLOCKED);
}

static bool needs_you(const orca_agent_t *a) {
    return a->state == ORCA_ST_WAITING || a->state == ORCA_ST_BLOCKED;
}

static bool same_agent(const orca_agent_t *a, const orca_agent_t *b) {
    return strcmp(a->type, b->type) == 0 && strcmp(a->repo, b->repo) == 0 &&
           strcmp(a->branch, b->branch) == 0;
}

bool orca_should_alert(const orca_board_t *prev, const orca_board_t *next) {
    for (uint8_t i = 0; i < next->count; i++) {
        if (!needs_you(&next->agents[i])) continue;
        bool already = false;
        if (prev && prev->received_ms) {
            for (uint8_t j = 0; j < prev->count && !already; j++)
                already = needs_you(&prev->agents[j]) && same_agent(&prev->agents[j], &next->agents[i]);
        }
        if (!already) return true;
    }
    return false;
}

const char *orca_state_label(uint8_t st) {
    switch (st) {
    case ORCA_ST_WORKING: return "工作中";
    case ORCA_ST_WAITING: return "等你处理";
    case ORCA_ST_BLOCKED: return "卡住了";
    case ORCA_ST_DONE: return "已完成";
    case ORCA_ST_IDLE: return "空闲";
    default: return "未知";
    }
}

void orca_format_age(uint32_t s, char *buf, size_t cap) {
    if (s < 60) snprintf(buf, cap, "%lu秒", (unsigned long)s);
    else if (s < 3600) snprintf(buf, cap, "%lu分钟", (unsigned long)(s / 60));
    else if (s < 86400) snprintf(buf, cap, "%lu小时", (unsigned long)(s / 3600));
    else snprintf(buf, cap, "%lu天", (unsigned long)(s / 86400));
}
