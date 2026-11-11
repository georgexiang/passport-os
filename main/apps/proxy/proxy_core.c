// main/apps/proxy/proxy_core.c —— 见 proxy_core.h。
#include "proxy_core.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

static const char *const kKeys[PROXY_MODE_COUNT] = {"rule", "global", "direct"};
static const char *const kLabels[PROXY_MODE_COUNT] = {"规则", "全局", "直连"};
static const char *const kEn[PROXY_MODE_COUNT] = {"RULE", "GLOBAL", "DIRECT"};

const char *proxy_mode_key(proxy_mode_t m)   { return m < PROXY_MODE_COUNT ? kKeys[m] : ""; }
const char *proxy_mode_label(proxy_mode_t m) { return m < PROXY_MODE_COUNT ? kLabels[m] : "--"; }
const char *proxy_mode_en(proxy_mode_t m)    { return m < PROXY_MODE_COUNT ? kEn[m] : "--"; }

static proxy_mode_t mode_from(const char *s) {
    for (int i = 0; i < PROXY_MODE_COUNT; i++)
        if (s && strcmp(s, kKeys[i]) == 0) return (proxy_mode_t)i;
    return PROXY_MODE_UNKNOWN;
}

void proxy_state_init(proxy_state_t *s) {
    memset(s, 0, sizeof(*s));
    s->remote.mode = PROXY_MODE_UNKNOWN;
    s->selected = PROXY_MODE_RULE;
}

static uint32_t num_u32(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsNumber(v) || v->valuedouble < 0) return 0;
    return v->valuedouble > 4294967295.0 ? UINT32_MAX : (uint32_t)v->valuedouble;
}

static bool flag(const cJSON *o, const char *k) {
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, k));
}

bool proxy_parse_state(const char *json, size_t len, proxy_remote_t *out) {
    if (!json || !out || len == 0 || len > 1024) return false;
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return false;
    bool ok = false;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *running = cJSON_GetObjectItemCaseSensitive(root, "running");
    if (cJSON_IsString(type) && strcmp(type->valuestring, "mihomo.state") == 0 &&
        cJSON_IsBool(running)) {
        memset(out, 0, sizeof(*out));
        out->running = cJSON_IsTrue(running);
        const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
        out->mode = mode_from(cJSON_IsString(mode) ? mode->valuestring : NULL);
        uint32_t port = num_u32(root, "port");
        out->port = port > 65535 ? 0 : (uint16_t)port;
        const cJSON *sp = cJSON_GetObjectItemCaseSensitive(root, "sysproxy");
        const char *sps = cJSON_IsString(sp) ? sp->valuestring : "";
        out->sysproxy = strcmp(sps, "on") == 0 ? PROXY_SYS_ON
                      : strcmp(sps, "mismatch") == 0 ? PROXY_SYS_MISMATCH
                      : strcmp(sps, "broken") == 0 ? PROXY_SYS_BROKEN : PROXY_SYS_OFF;
        out->up = num_u32(root, "up");
        out->down = num_u32(root, "down");
        out->busy = flag(root, "busy");
        const cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
        if (cJSON_IsString(err)) {
            strncpy(out->error, err->valuestring, sizeof(out->error) - 1);
            out->error[sizeof(out->error) - 1] = '\0';
        }
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

void proxy_apply_remote(proxy_state_t *s, const proxy_remote_t *r) {
    const bool first = !s->have_remote;
    const proxy_mode_t old_mode = s->remote.mode;
    s->remote = *r;
    s->have_remote = true;
    // Mac 正在执行启动/停止时继续等;否则这条就是请求的结果
    if (!r->busy) s->pending = false;
    // 首次收到、或模式在别处(网页面板)被改了 → 选中项跟上
    if (r->mode < PROXY_MODE_COUNT && (first || r->mode != old_mode)) s->selected = r->mode;
    if (!r->running) s->confirm_stop = false;
}

proxy_req_t proxy_on_key(proxy_state_t *s, proxy_key_t key, bool link_up,
                         uint64_t now_ms, proxy_mode_t *mode_out) {
    if (!link_up || !s->have_remote) return PROXY_REQ_NONE;
    if (s->pending || s->remote.busy) return PROXY_REQ_NONE;

    if (s->confirm_stop) {
        s->confirm_stop = false;
        if (key == PROXY_KEY_OK_CLICK) {
            s->pending = true;
            s->pending_since_ms = now_ms;
            return PROXY_REQ_STOP;
        }
        return PROXY_REQ_NONE;   // 其他键 = 取消
    }

    switch (key) {
    case PROXY_KEY_UP_CLICK:
        s->selected = (proxy_mode_t)((s->selected + PROXY_MODE_COUNT - 1) % PROXY_MODE_COUNT);
        return PROXY_REQ_NONE;
    case PROXY_KEY_DOWN_CLICK:
        s->selected = (proxy_mode_t)((s->selected + 1) % PROXY_MODE_COUNT);
        return PROXY_REQ_NONE;
    case PROXY_KEY_OK_CLICK:
        if (!s->remote.running) {   // 未运行时确定 = 启动(和长按下键一样)
            s->pending = true;
            s->pending_since_ms = now_ms;
            return PROXY_REQ_START;
        }
        if (s->selected == s->remote.mode) return PROXY_REQ_NONE;
        s->pending = true;
        s->pending_since_ms = now_ms;
        if (mode_out) *mode_out = s->selected;
        return PROXY_REQ_SET_MODE;
    case PROXY_KEY_DOWN_LONG:
        if (s->remote.running) {
            s->confirm_stop = true;
            s->confirm_since_ms = now_ms;
            return PROXY_REQ_NONE;
        }
        s->pending = true;
        s->pending_since_ms = now_ms;
        return PROXY_REQ_START;
    }
    return PROXY_REQ_NONE;
}

void proxy_tick(proxy_state_t *s, uint64_t now_ms) {
    if (s->pending && now_ms - s->pending_since_ms >= PROXY_PENDING_TIMEOUT_MS) {
        s->pending = false;
        if (s->remote.mode < PROXY_MODE_COUNT) s->selected = s->remote.mode;
    }
    if (s->confirm_stop && now_ms - s->confirm_since_ms >= PROXY_CONFIRM_TIMEOUT_MS)
        s->confirm_stop = false;
}

size_t proxy_build_request(char *buf, size_t cap, proxy_req_t req, proxy_mode_t mode) {
    int n;
    switch (req) {
    case PROXY_REQ_SET_MODE:
        if (mode >= PROXY_MODE_COUNT) return 0;
        n = snprintf(buf, cap, "{\"event\":\"mihomo.set_mode\",\"mode\":\"%s\"}\n", kKeys[mode]);
        break;
    case PROXY_REQ_START: n = snprintf(buf, cap, "{\"event\":\"mihomo.start\"}\n"); break;
    case PROXY_REQ_STOP:  n = snprintf(buf, cap, "{\"event\":\"mihomo.stop\"}\n"); break;
    case PROXY_REQ_QUERY: n = snprintf(buf, cap, "{\"event\":\"mihomo.query\"}\n"); break;
    default: return 0;
    }
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

void proxy_format_rate(uint32_t bps, char *buf, size_t cap) {
    if (bps >= 1048576u)
        snprintf(buf, cap, "%lu.%lu MB/s", (unsigned long)(bps / 1048576u),
                 (unsigned long)((bps % 1048576u) * 10u / 1048576u));
    else if (bps >= 1024u)
        snprintf(buf, cap, "%lu.%lu KB/s", (unsigned long)(bps / 1024u),
                 (unsigned long)((bps % 1024u) * 10u / 1024u));
    else
        snprintf(buf, cap, "%lu B/s", (unsigned long)bps);
}
