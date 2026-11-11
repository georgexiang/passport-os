// WeType remote: UP records, DOWN sends Enter after recognition.
// OK click toggles cursor mode: UP/DOWN move the PC cursor instead of recording.
#include "app_state.h"
#include <stdio.h>
#include <string.h>

#define PC_STATUS_TIMEOUT_MS 3500u
#define UP_PRESS_MAX_MV 400u
static void emit(app_action_t *out, uint8_t *n, app_action_type_t type) {
    if (*n < APP_ACT_MAX) out[(*n)++] = (app_action_t){ .type = type };
}
static void toast(app_state_t *s, uint64_t now, const char *msg) {
    snprintf(s->toast, sizeof(s->toast), "%s", msg);
    s->toast_until_ms = now + 1500;
}
static void send_key(app_action_t *out, uint8_t *n, app_key_action_t action) {
    if (*n < APP_ACT_MAX) out[(*n)++] = (app_action_t){.type = APP_ACT_SEND_KEY_ACTION,
        .u.key_action.action = (uint8_t)action};
}
static void end_voice(app_state_t *s, uint64_t now, bool cancel, app_action_t *out, uint8_t *n) {
    if (s->state != APP_ST_LISTENING) return;
    emit(out, n, cancel ? APP_ACT_STREAM_CANCEL : APP_ACT_STREAM_STOP);
    emit(out, n, APP_ACT_SEND_VOICE_END);
    s->state = APP_ST_READY;
    s->state_since_ms = now;
    s->stream_started = false;
    s->net_busy = false;
}
void app_state_init(app_state_t *s) {
    memset(s, 0, sizeof(*s));
    s->state = APP_ST_READY;
    s->screen_on = s->panel_on = true;
}
void app_state_snapshot(const app_state_t *s, uint64_t now, app_ui_snapshot_t *snap) {
    memset(snap, 0, sizeof(*snap));
    snap->state = s->state;
    snap->link_up = s->link_up;
    snap->pc_ready = s->pc_ready;
    snap->init_failed = s->init_failed;
    snap->screen_on = s->screen_on;
    snap->panel_on = s->panel_on;
    snap->net_busy = s->net_busy;
    snap->cursor_mode = s->cursor_mode;
    snap->elapsed_ms = (uint32_t)(now - s->state_since_ms);
    snprintf(snap->link_name, sizeof(snap->link_name), "WiFi");
    if (now < s->toast_until_ms) snprintf(snap->toast, sizeof(snap->toast), "%s", s->toast);
}
void app_state_reduce(app_state_t *s, const app_event_t *ev, uint64_t now, app_action_t *out, uint8_t *n) {
    *n = 0;
    if (ev->type == APP_EV_INIT_FAILED) {
        end_voice(s, now, true, out, n);
        s->init_failed = true;
        s->link_up = s->pc_ready = false;
        return;
    }
    // Late beacons/status/keys cannot revive a failed local session.
    if (s->init_failed) return;
    switch (ev->type) {
    case APP_EV_LINK_UP: s->link_up = true; break;
    case APP_EV_LINK_DOWN:
        s->link_up = s->pc_ready = false;
        s->pc_status_ms = 0;
        end_voice(s, now, true, out, n);
        break;
    case APP_EV_PC_STATUS:
        s->pc_status_ms = now;
        s->pc_ready = ev->u.pc_status.audio_ready && ev->u.pc_status.hotkey_ready;
        if (!s->pc_ready) end_voice(s, now, true, out, n);
        break;
    case APP_EV_KEY_PRESS:
        // ADC guard rejects cross-key ghosts. A fresh press is required after failures.
        if (ev->u.key.btn == APP_BTN_UP && ev->u.key.mv <= UP_PRESS_MAX_MV && !s->cursor_mode &&
            s->state == APP_ST_READY && s->link_up && s->pc_ready) {
            s->state = APP_ST_LISTENING;
            s->state_since_ms = now;
            s->stream_started = true;
            s->toast_until_ms = 0;
            emit(out, n, APP_ACT_SEND_VOICE_START);
            emit(out, n, APP_ACT_STREAM_START);
        }
        break;
    case APP_EV_KEY_RELEASE:
    case APP_EV_KEY_LONG_UP:
        if (ev->u.key.btn == APP_BTN_UP) end_voice(s, now, false, out, n);
        break;
    case APP_EV_KEY_CLICK:
        // CLICK is emitted once on a short press. Never send while still recording
        // or after the PC heartbeat expires. Release ADC readings are not key IDs.
        if (s->state != APP_ST_READY) break;
        if (ev->u.key.btn == APP_BTN_OK) {  // OK long press stays the registry's global "back"
            s->cursor_mode = !s->cursor_mode;
            toast(s, now, s->cursor_mode ? "光标模式" : "语音模式");
            break;
        }
        if (!s->link_up || !s->pc_ready) break;
        if (s->cursor_mode) {
            if (ev->u.key.btn == APP_BTN_UP) send_key(out, n, APP_KEY_LEFT);
            else if (ev->u.key.btn == APP_BTN_DOWN) send_key(out, n, APP_KEY_RIGHT);
        } else if (ev->u.key.btn == APP_BTN_DOWN) {
            send_key(out, n, APP_KEY_ENTER);
            toast(s, now, "已发送回车");
        }
        break;
    case APP_EV_KEY_LONG:
        if (s->state != APP_ST_READY || !s->link_up || !s->pc_ready) break;
        if (s->cursor_mode) {
            if (ev->u.key.btn == APP_BTN_UP) { send_key(out, n, APP_KEY_HOME); toast(s, now, "行首"); }
            else if (ev->u.key.btn == APP_BTN_DOWN) { send_key(out, n, APP_KEY_END); toast(s, now, "行尾"); }
        } else if (ev->u.key.btn == APP_BTN_DOWN) {
            send_key(out, n, APP_KEY_UNDO);
            toast(s, now, "已撤销");
        }
        break;
    case APP_EV_AUDIO_ERROR:
        end_voice(s, now, true, out, n);
        snprintf(s->toast, sizeof(s->toast), "麦克风异常，请重试");
        s->toast_until_ms = now + 4000;
        break;
    case APP_EV_AUDIO_DROP_START: s->net_busy = s->state == APP_ST_LISTENING; break;
    case APP_EV_AUDIO_DROP_END: s->net_busy = false; break;
    case APP_EV_TICK:
        if (s->pc_ready && now - s->pc_status_ms > PC_STATUS_TIMEOUT_MS) {
            s->pc_ready = false;
            end_voice(s, now, true, out, n);
        }
        if (s->state == APP_ST_LISTENING && now - s->state_since_ms >= APP_PTT_MAX_TALK_MS) {
            end_voice(s, now, false, out, n);
            snprintf(s->toast, sizeof(s->toast), "已暂停，请松开上键");
            s->toast_until_ms = now + 4000;
        }
        break;
    case APP_EV_TIME_SET:
        out[(*n)++] = (app_action_t){.type = APP_ACT_TIME_SET, .u.time_set.epoch = ev->u.time_set.epoch};
        break;
    default:
        // No clear, approval or transcript. Registry owns exit and sleep.
        break;
    }
}
