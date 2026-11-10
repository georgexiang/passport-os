// main/apps/away/app_away.cc —— 「离席哨」:离开电脑时,替你盯飞书(含静音群)的未读。
//
// Mac 端 Passport Voice 在"离开"状态下每 5 分钟读一次飞书,有未读就下发(协议见 away_core.h)。
// 两种通道二选一(长按下键切换,记在 NVS 里):
//   Wi-Fi  复用语音应用的 udp_audio 通道(UDP 33333),需同一路由器
//   蓝牙   和 Orca 看板共用的 BLE GATT 服务(link_ble.h),Mac 直连,不需要 Wi-Fi
// 消息格式两种通道完全一样。
//
// 按键:
//   确定 单击  切换 离开 / 回来(同步给 Mac;回来时 Mac 停止查询并清空)
//   上 / 下    翻看未读列表
//   长按下键   切换 Wi-Fi / 蓝牙 通道
//   任意键     停止正在响的提醒
//   长按确定   整机统一的"返回"
// 新增未读(新的群,或某个群条数变多):亮屏 + 提醒音。
#include "apps/app_base.h"
#include "apps/away/away_core.h"
#include "apps/link/link_ble.h"
#include "apps/voice/udp_audio.h"
#include "launcher/app_registry.h"
#include "app_fonts.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "ui_pixel.h"
#include "ui_sfx.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstdio>
#include <cstring>
#include <ctime>

namespace {
constexpr const char *kTag = "away";
constexpr int kRows = 6;
constexpr int kRowY = 96;
constexpr int kRowH = 28;
constexpr uint64_t kStateStaleMs = 8000;
constexpr uint64_t kRemindMs = 60000;      // 未读没处理:每分钟再响一次,直到按键或回来

// 快照缓冲只在应用运行时分配:staging(RX 任务拼包)、inbox(拼好待取)、board(显示)
struct Mem { away_board_t staging, inbox, board; };
Mem *s_mem = nullptr;
SemaphoreHandle_t s_lock = nullptr;
StaticSemaphore_t s_lock_buf;
bool s_inbox_new = false;
// 离开状态:RX 任务写,LVGL 读,同样受 s_lock 保护
bool s_rx_away = false;
uint32_t s_rx_since = 0;
uint64_t s_rx_state_ms = 0;

bool s_away = false;        // 当前显示的状态
uint32_t s_since = 0;
uint64_t s_state_ms = 0;
uint8_t s_top = 0;          // 列表滚动位置
bool s_sound_pending = false;
bool s_unacked = false;     // 有未读还没按键确认
uint64_t s_last_ring_ms = 0;
bool s_transport_up = false;
bool s_ble = false;         // 当前通道:false = Wi-Fi UDP,true = 蓝牙


lv_obj_t *s_screen;
lv_timer_t *s_timer;
lv_obj_t *s_wifi_bars[3];
ui_pixel_battery_t s_battery{};
uint64_t s_battery_updated_us;
lv_obj_t *s_banner, *s_status, *s_sub, *s_empty, *s_footer, *s_mode;
lv_obj_t *s_row_bg[kRows], *s_row_dot[kRows], *s_row_name[kRows], *s_row_count[kRows];

uint64_t now_ms() { return static_cast<uint64_t>(esp_timer_get_time() / 1000); }
#define s_board (s_mem->board)

// ---- 通道 ----
void on_event_line(const char *json, size_t len) {
    // RX 任务里:持锁解析,stop() 释放缓冲前也要拿这把锁。
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return;
    Mem *m = s_mem;
    bool away = s_rx_away;
    uint32_t since = s_rx_since;
    const away_msg_t r = m ? away_parse(json, len, now_ms(), &m->staging, &away, &since) : AWAY_MSG_NONE;
    if (r == AWAY_MSG_STATE) {
        s_rx_away = away; s_rx_since = since; s_rx_state_ms = now_ms();
    } else if (r == AWAY_MSG_COMPLETE) {
        m->inbox = m->staging;
        s_inbox_new = true;
    }
    xSemaphoreGive(s_lock);
    if (r == AWAY_MSG_BAD) ESP_LOGW(kTag, "bad line: %.*s", (int)(len > 60 ? 60 : len), json);
}

void send_line(const char *line) {
    if (s_ble) link_ble_send(line, std::strlen(line));
    else udp_audio_notify_event(line, std::strlen(line));
}

// ---- 通道开关 ----
bool transport_open() {
    if (s_ble) return link_ble_start(on_event_line);
    udp_audio_set_event_handler(on_event_line);   // 先挂处理函数再开 socket
    if (udp_audio_init() == 0) return true;
    udp_audio_set_event_handler(nullptr);
    return false;
}
void transport_close() {
    if (s_ble) {
        link_ble_stop();
    } else {
        udp_audio_deinit();                       // 先关 socket 再撤处理函数(udp_audio 的约定)
        udp_audio_set_event_handler(nullptr);
    }
}
void send_hello() { send_line("{\"event\":\"away.hello\"}\n"); }
void send_set(bool away) {
    send_line(away ? "{\"event\":\"away.set\",\"away\":true}\n" : "{\"event\":\"away.set\",\"away\":false}\n");
}

// ---- 界面构件 ----
lv_obj_t *label(const char *text, const lv_font_t *font, uint32_t color, int x, int y, int w, int h) {
    lv_obj_t *o = ui_pixel_label(s_screen, text, font, color);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_label_set_long_mode(o, LV_LABEL_LONG_DOT);
    return o;
}
lv_obj_t *box(int x, int y, int w, int h, uint32_t color) {
    lv_obj_t *o = lv_obj_create(s_screen);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    return o;
}
void hide(lv_obj_t *o, bool h) { lv_obj_set_flag(o, LV_OBJ_FLAG_HIDDEN, h); }

void render_status_bar(uint64_t now_us) {
    if (s_mode) {   // 通道标签:蓝牙模式下 Wi-Fi 信号格没有意义,用文字说明
        ui_pixel_label_set_text(s_mode, s_ble ? "蓝牙" : "Wi-Fi");
        ui_pixel_text_color(s_mode, s_ble ? UI_ACCENT : UI_TEXT_DIM);
        for (lv_obj_t *b : s_wifi_bars) if (b) hide(b, s_ble);
    }
    if (s_battery_updated_us && now_us - s_battery_updated_us < 5000000) return;
    ui_pixel_battery_set(&s_battery, bsp_battery_soc());
    auto &wifi = WifiManager::GetInstance();
    const bool ok = wifi.IsConnected();
    const int rssi = ok ? wifi.GetRssi() : -127;
    ui_pixel_wifi_bars_set(s_wifi_bars, !ok ? 0 : rssi >= -60 ? 3 : rssi >= -75 ? 2 : 1,
                           ok ? UI_TEXT : UI_TEXT_DIM);
    s_battery_updated_us = now_us;
}

// ---- 渲染 ----
void render() {
    if (!s_screen || !s_mem) return;
    const uint64_t now = now_ms();
    const bool linked = s_transport_up && s_state_ms && now - s_state_ms <= kStateStaleMs;
    const bool have = linked && s_away && s_board.count > 0;

    // 顶部横幅:一眼分清四种状态
    //   红(未确认时闪烁) = 有未读要处理    绿 = 离开中,没有未读
    //   灰 = 正在查询 / 在电脑前 / 没连上电脑
    const time_t wall = time(nullptr);
    const uint32_t total = linked && s_away ? away_unread_total(&s_board) : 0;
    const bool checked = linked && s_away && s_board.checked;
    char head[48], line[128], dur[32] = "", chk[32] = "";
    if (s_since && wall > (time_t)s_since) away_format_duration((uint32_t)(wall - s_since), dur, sizeof(dur));
    if (s_board.checked && wall >= (time_t)s_board.checked)
        away_format_duration((uint32_t)(wall - s_board.checked), chk, sizeof(chk));
    const char *ago = std::strcmp(chk, "刚刚") ? "前" : "";
    uint32_t bg = UI_SURFACE_ALT, fg = UI_TEXT;
    if (!s_transport_up) {
        std::snprintf(head, sizeof(head), s_ble ? "蓝牙启动失败" : "网络未就绪");
        std::snprintf(line, sizeof(line), s_ble ? "内存不足,长按下键换 Wi-Fi" : "请先连接 Wi-Fi");
    } else if (!linked) {
        std::snprintf(head, sizeof(head), "等待电脑连接");
        std::snprintf(line, sizeof(line), s_ble ? "蓝牙广播中 · Passport Voice" : "打开 Mac 上的 Passport Voice");
    } else if (!s_away) {
        std::snprintf(head, sizeof(head), "在电脑前");
        std::snprintf(line, sizeof(line), "按确定设为离开");
    } else if (total) {
        // 未确认时红底/暗底交替闪烁;按过键后常亮红底
        const bool flash_off = s_unacked && (now / 500) % 2;
        bg = flash_off ? UI_BG : UI_DANGER;
        fg = flash_off ? UI_DANGER : UI_BG;   // 亮底配深色字,读得清
        std::snprintf(head, sizeof(head), "有 %u 条未读", (unsigned)total);
        std::snprintf(line, sizeof(line), "%u 个群 · %s%s查过", s_board.total, chk, ago);
    } else if (checked) {
        bg = UI_SUCCESS; fg = UI_BG;
        std::snprintf(head, sizeof(head), "没有未读消息");
        std::snprintf(line, sizeof(line), "%s%s查过", chk, ago);
    } else {
        std::snprintf(head, sizeof(head), "离开中 · 正在查询");
        std::snprintf(line, sizeof(line), "已离开 %s", dur[0] ? dur : "刚刚");
    }
    ui_pixel_bg_color(s_banner, bg);
    // 闪烁的暗相位也留一圈红框,横幅不会"消失"
    lv_obj_set_style_border_width(s_banner, bg == UI_BG ? 3 : 0, 0);
    lv_obj_set_style_border_color(s_banner, lv_color_hex(UI_DANGER), 0);
    ui_pixel_label_set_text(s_status, head);
    ui_pixel_text_color(s_status, fg);
    ui_pixel_label_set_text(s_sub, line);
    ui_pixel_text_color(s_sub, fg);

    hide(s_empty, have);
    char empty_note[96];
    std::snprintf(empty_note, sizeof(empty_note), "飞书里没有\n需要处理的消息\n\n已离开 %s", dur[0] ? dur : "刚刚");
    if (!have) {
        ui_pixel_label_set_text(s_empty,
            !s_transport_up ? (s_ble ? "长按下键切换到 Wi-Fi" : "请先连接 Wi-Fi\n或长按下键改用蓝牙") :
            !linked ? "在电脑上打开 Passport Voice" :
            !s_away ? "离开时会每 5 分钟\n查一次飞书未读\n(含静音群)" :
            checked ? empty_note : "正在读取飞书…");
    }

    if (s_top + kRows > s_board.count) s_top = s_board.count > kRows ? s_board.count - kRows : 0;
    for (int i = 0; i < kRows; i++) {
        const int idx = s_top + i;
        const bool show = have && idx < s_board.count;
        hide(s_row_bg[i], !show); hide(s_row_dot[i], !show);
        hide(s_row_name[i], !show); hide(s_row_count[i], !show);
        if (!show) continue;
        const away_item_t &it = s_board.items[idx];
        // 灰点 = 静音群,红点 = 普通群
        ui_pixel_bg_color(s_row_dot[i], it.muted ? UI_TEXT_DIM : UI_DANGER);
        // 来源放前面:群名太长会被截掉,来源(哪个账户)不能丢
        if (it.source[0]) std::snprintf(line, sizeof(line), "%s·%s", it.source, it.name);
        else std::snprintf(line, sizeof(line), "%s", it.name);
        ui_pixel_label_set_text(s_row_name[i], line);
        std::snprintf(line, sizeof(line), "%u", it.count);
        ui_pixel_label_set_text(s_row_count[i], line);
        ui_pixel_text_color(s_row_count[i], it.muted ? UI_TEXT_DIM : UI_DANGER);
        ui_pixel_bg_color(s_row_bg[i], i % 2 ? UI_SURFACE_ALT : UI_BG);
    }

    ui_pixel_label_set_text(s_footer, !linked ? (s_ble ? "长按下键:换 Wi-Fi" : "长按下键:换蓝牙") :
        s_away ? (s_board.count > kRows ? "确定:回来 · 上下翻看" : "确定:我回来了") : "确定:我要离开");
    render_status_bar(esp_timer_get_time());
}

void tick(lv_timer_t *) {
    if (!s_mem) return;
    // got  = 要重画;wake = 真有变化,续期熄屏计时。
    // Mac 每 5 秒回应 away.hello 时会重发同一份快照,那不算"有变化",否则屏幕永远不熄。
    bool got = false, alert = false, wake = false;
    if (xSemaphoreTake(s_lock, 0) == pdTRUE) {
        if (s_rx_state_ms != s_state_ms) {
            if (s_rx_away != s_away) {
                // 状态变了(Mac 菜单切换,或回应设备的切换)。回来时清空列表、停铃。
                if (!s_rx_away) { s_board = away_board_t{}; s_top = 0; }
                got = wake = true;
            }
            s_away = s_rx_away; s_since = s_rx_since; s_state_ms = s_rx_state_ms;
        }
        if (s_inbox_new) {
            alert = s_away && away_should_alert(&s_board, &s_mem->inbox);
            wake = alert;
            s_board = s_mem->inbox;
            s_inbox_new = false;
            got = true;
        }
        xSemaphoreGive(s_lock);
    }
    if (!s_away || away_unread_total(&s_board) == 0) s_unacked = false;
    else if (alert) s_unacked = true;
    // 没确认的未读:隔一分钟再响一次(只响铃,不点亮屏幕,屏幕照常自动熄灭)
    bool remind = false;
    if (!alert && s_unacked && !s_sound_pending && now_ms() - s_last_ring_ms >= kRemindMs) remind = true;
    if (alert || remind) {
        s_sound_pending = true;
        s_last_ring_ms = now_ms();
    }
    if (alert) {
        app_registry_wake_for_notice();   // 定时器在 LVGL 任务里,已持有 LVGL 锁
        ESP_LOGI(kTag, "new unread: %u", (unsigned)away_unread_total(&s_board));
    }
    if (wake) app_registry_keep_awake();
    if (s_sound_pending) {
        if (!s_away) { ui_sfx_cancel_alarm(); s_sound_pending = false; }
        else if (app_registry_sound_notice()) s_sound_pending = false;
    }
    static uint64_t last_hello;
    const uint64_t now = now_ms();
    if (s_transport_up && now - last_hello >= 5000) { send_hello(); last_hello = now; }
    if (app_registry_screen_is_on() || got) render();
}

// ---- 生命周期 ----
void init() {}

void start() {
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    if (!s_mem) {
        s_mem = static_cast<Mem *>(heap_caps_calloc(1, sizeof(Mem), MALLOC_CAP_8BIT));
        if (!s_mem) { ESP_LOGE(kTag, "no memory for %u-byte board", (unsigned)sizeof(Mem)); return; }
    }
    s_inbox_new = false;
    s_rx_away = s_away = false;
    s_rx_since = s_since = 0;
    s_rx_state_ms = s_state_ms = 0;
    s_ble = link_ble_mode();
    s_transport_up = transport_open();
    if (!s_transport_up) ESP_LOGE(kTag, "%s channel unavailable", s_ble ? "BLE" : "UDP");
    if (!bsp_lvgl_lock(1000)) return;
    if (s_screen) { bsp_lvgl_unlock(); return; }
    s_top = 0; s_sound_pending = false; s_unacked = false; s_last_ring_ms = 0;

    s_screen = lv_obj_create(nullptr);
    ui_pixel_background(s_screen);
    lv_obj_t *top = ui_pixel_top_bar(s_screen);
    ui_pixel_wifi_bars_create(top, 12, 18, s_wifi_bars);
    lv_obj_t *name = ui_pixel_label(top, "离席哨", &buddy_font_16, UI_TEXT);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 42, 0);
    s_battery = ui_pixel_battery_create(top, 206, 6, UI_TEXT, &lv_font_montserrat_14);
    s_mode = ui_pixel_label(top, "", &buddy_font_16, UI_TEXT_DIM);
    lv_obj_align(s_mode, LV_ALIGN_LEFT_MID, 108, 0);

    s_banner = box(6, 36, 228, 54, UI_SURFACE_ALT);
    s_status = label("", &away_font_20, UI_TEXT, 14, 40, 212, 26);
    s_sub = label("", &buddy_font_16, UI_TEXT, 14, 66, 212, 20);
    s_empty = label("", &buddy_font_16, UI_TEXT_DIM, 12, 130, 216, 96);
    lv_label_set_long_mode(s_empty, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_empty, LV_TEXT_ALIGN_CENTER, 0);

    for (int i = 0; i < kRows; i++) {
        const int y = kRowY + i * kRowH;
        s_row_bg[i] = box(8, y, 224, kRowH - 2, UI_BG);
        s_row_dot[i] = box(14, y + 9, 8, 8, UI_DANGER);
        s_row_name[i] = label("", &buddy_font_16, UI_TEXT, 28, y + 3, 168, 22);
        s_row_count[i] = label("", &buddy_font_16, UI_DANGER, 196, y + 3, 32, 22);
        lv_obj_set_style_text_align(s_row_count[i], LV_TEXT_ALIGN_RIGHT, 0);
    }

    s_footer = ui_pixel_footer(s_screen, "", "长按确定返回", &buddy_font_16);
    s_battery_updated_us = 0;
    render();
    lv_screen_load(s_screen);
    s_timer = lv_timer_create(tick, 250, nullptr);
    ESP_LOGI(kTag, "started; %s=%d", s_ble ? "ble" : "udp", s_transport_up);
    bsp_lvgl_unlock();
    if (s_transport_up) send_hello();
}

void stop() {
    if (bsp_lvgl_lock(-1)) {
        if (s_sound_pending) ui_sfx_cancel_alarm();
        s_sound_pending = false;
        if (s_timer) { lv_timer_delete(s_timer); s_timer = nullptr; }
        if (s_screen) { lv_obj_delete(s_screen); s_screen = nullptr; }
        s_wifi_bars[0] = s_wifi_bars[1] = s_wifi_bars[2] = nullptr;
        s_mode = nullptr;
        s_battery = {};
        bsp_lvgl_unlock();
    }
    transport_close();
    s_transport_up = false;
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        Mem *m = s_mem;
        s_mem = nullptr;
        s_inbox_new = false;
        xSemaphoreGive(s_lock);
        heap_caps_free(m);
    }
}

// 长按下键:换通道。先关旧的(释放内存),再开新的;离开状态在 Mac 端,不受影响。
void switch_transport() {
    transport_close();
    s_ble = !s_ble;
    link_ble_set_mode(s_ble);
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_rx_state_ms = 0;
        xSemaphoreGive(s_lock);
    }
    s_state_ms = 0;
    s_transport_up = transport_open();
    ESP_LOGI(kTag, "transport -> %s (%s)", s_ble ? "BLE" : "Wi-Fi", s_transport_up ? "ok" : "failed");
    if (s_transport_up) send_hello();
}

void on_key(uint8_t btn, app_btn_event_t event, uint16_t) {
    if (btn == BSP_BTN_DOWN && event == BTN_EVT_LONG) {
        switch_transport();   // 不持 LVGL 锁:关 NimBLE 要等它的任务退出
        if (bsp_lvgl_lock(500)) { render(); bsp_lvgl_unlock(); }
        return;
    }
    if (event != BTN_EVT_CLICK || !bsp_lvgl_lock(500)) return;
    if (s_screen && s_mem) {
        const bool linked = s_transport_up && s_state_ms && now_ms() - s_state_ms <= kStateStaleMs;
        if (btn == BSP_BTN_OK && linked) {
            // 先在本地切换并显示,Mac 收到后回发 away.state 确认
            s_away = !s_away;
            if (!s_away) { s_board = away_board_t{}; s_top = 0; }
            // 同步 RX 侧,免得下一个旧的 away.state 把刚切的状态又改回去
            if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
                s_rx_away = s_away;
                if (s_away) s_rx_since = s_since = static_cast<uint32_t>(time(nullptr));
                xSemaphoreGive(s_lock);
            }
            send_set(s_away);
        } else if (btn == BSP_BTN_UP && s_top > 0) {
            s_top--;
        } else if (btn == BSP_BTN_DOWN && s_top + kRows < s_board.count) {
            s_top++;
        }
        // 按任何键 = 看到了:停铃,不再重复提醒(直到又有新增)
        if (s_sound_pending) { ui_sfx_cancel_alarm(); s_sound_pending = false; }
        s_unacked = false;
        render();
    }
    bsp_lvgl_unlock();
}
} // namespace

extern const passport_app_t g_away_app = {
    .id = APP_ID_AWAY,
    .name = "离席哨",
    .en_name = "AWAY",
    .desc = "离开时盯飞书未读\n需电脑转发",
    .tag = "需电脑",
    .theme_color = 0xE8743B,
    .init = init,
    .start = start,
    .stop = stop,
    .on_key = on_key,
};
