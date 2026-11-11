// main/apps/proxy/app_proxy.cc —— 「代理开关」:查看/控制 Mac 上的 mihomo。
// 通道复用随身语音的 Wi-Fi UDP 33333(udp_audio),Mac 端是 Passport Voice。
// 纯逻辑在 proxy_core.c;本文件只做生命周期、按键映射和 LVGL 界面。
//
// 线程:
//   RX 任务(udp_audio)→ on_udp_event():只解析 + 存入 s_inbox(加锁),不碰 LVGL
//   LVGL 定时器 tick():取 s_inbox → proxy_apply_remote → 渲染
//   input 任务 on_key():持 LVGL 锁改状态、发请求、渲染
#include "apps/app_base.h"
#include "apps/proxy/proxy_core.h"
#include "apps/voice/udp_audio.h"
#include "launcher/app_registry.h"
#include "bsp_display.h"
#include "bsp_battery.h"
#include "app_fonts.h"
#include "ui_pixel.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <cstdio>
#include <cstring>

namespace {
const char *TAG = "app_proxy";

proxy_state_t s_state;
// RX 任务 → LVGL 定时器 的单槽信箱(只保留最新一条)
portMUX_TYPE s_inbox_mux = portMUX_INITIALIZER_UNLOCKED;
proxy_remote_t s_inbox;
bool s_inbox_full = false;
uint64_t s_last_query_ms = 0;
bool s_transport_ok = false;

lv_obj_t *s_screen;
lv_timer_t *s_timer;
lv_obj_t *s_link, *s_badge, *s_badge_cn, *s_badge_en, *s_run, *s_rates, *s_sys;
lv_obj_t *s_choices[PROXY_MODE_COUNT];
lv_obj_t *s_notice, *s_footer;
lv_obj_t *s_wifi_bars[3];
ui_pixel_battery_t s_battery{};
uint64_t s_status_updated_us;

uint64_t now_ms() { return esp_timer_get_time() / 1000; }

// 模式颜色:规则蓝 / 全局橙 / 直连灰
uint32_t mode_color(proxy_mode_t m) {
    switch (m) {
    case PROXY_MODE_RULE: return 0x4C8DFF;
    case PROXY_MODE_GLOBAL: return 0xF08A2C;
    case PROXY_MODE_DIRECT: return 0x8A8F98;
    default: return 0x3A3D44;
    }
}

void on_udp_event(const char *json, size_t len) {
    proxy_remote_t r;
    if (!proxy_parse_state(json, len, &r)) return;   // 其他消息(time.set、pc.status 等)忽略
    portENTER_CRITICAL(&s_inbox_mux);
    s_inbox = r;
    s_inbox_full = true;
    portEXIT_CRITICAL(&s_inbox_mux);
}

void send_request(proxy_req_t req, proxy_mode_t mode) {
    char buf[96];
    size_t n = proxy_build_request(buf, sizeof(buf), req, mode);
    if (n && udp_audio_notify_event(buf, n) != 0) ESP_LOGW(TAG, "请求发送失败: %.*s", (int)n - 1, buf);
}

lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color,
                int x, int y, int w, int h, lv_text_align_t align) {
    lv_obj_t *o = ui_pixel_label(parent, text, font, color);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_label_set_long_mode(o, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(o, align, 0);
    return o;
}

void render() {
    if (!s_screen) return;
    const proxy_state_t &s = s_state;
    const proxy_remote_t &r = s.remote;
    const bool link = s_transport_ok && udp_audio_connected();

    ui_pixel_label_set_text(s_link, !s_transport_ok ? "通道失败" : link ? "已连接" : "未连接");
    ui_pixel_text_color(s_link, link ? UI_SUCCESS : UI_TEXT_DIM);

    // 大色块:当前模式
    const bool known = link && s.have_remote && r.running;
    const proxy_mode_t cur = known ? r.mode : PROXY_MODE_UNKNOWN;
    ui_pixel_bg_color(s_badge, mode_color(cur));
    ui_pixel_label_set_text(s_badge_cn, known ? proxy_mode_label(cur) : (link && s.have_remote ? "已停止" : "--"));
    ui_pixel_label_set_text(s_badge_en, known ? proxy_mode_en(cur) : (link && s.have_remote ? "STOPPED" : "OFFLINE"));

    // 运行状态 / 速度
    char line[64], up[20], down[20];
    if (!link) {
        ui_pixel_label_set_text(s_run, "打开 Mac 上的 Passport Voice");
        ui_pixel_label_set_text(s_rates, "");
    } else if (!s.have_remote) {
        ui_pixel_label_set_text(s_run, "等待 mihomo 状态…");
        ui_pixel_label_set_text(s_rates, "");
    } else if (s.pending || r.busy) {
        ui_pixel_label_set_text(s_run, "处理中…");
    } else if (r.running) {
        std::snprintf(line, sizeof(line), "mihomo 运行中 · 端口 %u", (unsigned)r.port);
        ui_pixel_label_set_text(s_run, line);
    } else {
        ui_pixel_label_set_text(s_run, "mihomo 未运行");
    }
    if (link && s.have_remote && r.running) {
        proxy_format_rate(r.up, up, sizeof(up));
        proxy_format_rate(r.down, down, sizeof(down));
        std::snprintf(line, sizeof(line), "UP %s  DN %s", up, down);
        ui_pixel_label_set_text(s_rates, line);
    } else if (link) {
        ui_pixel_label_set_text(s_rates, "");
    }

    // 系统代理
    const char *sys = "系统代理 --";
    uint32_t sys_col = UI_TEXT_DIM;
    if (link && s.have_remote) {
        switch (r.sysproxy) {
        case PROXY_SYS_ON: sys = "系统代理 开"; sys_col = UI_SUCCESS; break;
        case PROXY_SYS_OFF: sys = "系统代理 关"; break;
        case PROXY_SYS_MISMATCH: sys = "系统代理 端口不一致"; sys_col = UI_WARNING; break;
        case PROXY_SYS_BROKEN: sys = "系统代理开着 会断网"; sys_col = UI_DANGER; break;
        }
    }
    ui_pixel_label_set_text(s_sys, sys);
    ui_pixel_text_color(s_sys, sys_col);

    // 模式选择行
    const bool can_pick = link && s.have_remote && r.running && !s.pending && !r.busy;
    for (int i = 0; i < PROXY_MODE_COUNT; i++) {
        const bool sel = can_pick && s.selected == i;
        ui_pixel_bg_color(s_choices[i], sel ? mode_color((proxy_mode_t)i) : UI_SURFACE);
        lv_obj_t *txt = lv_obj_get_child(s_choices[i], 0);
        ui_pixel_text_color(txt, sel ? 0xFFFFFF : (can_pick ? UI_TEXT : UI_TEXT_DIM));
        lv_obj_set_style_border_width(s_choices[i], r.mode == i && known ? 2 : 0, 0);
    }

    // 提示行
    const char *notice = "";
    uint32_t notice_col = UI_TEXT_DIM;
    if (s.confirm_stop) { notice = "再按确定 停止 mihomo"; notice_col = UI_DANGER; }
    else if (link && s.have_remote && r.error[0]) { notice = r.error; notice_col = UI_WARNING; }
    else if (can_pick && s.selected != r.mode) { notice = "按确定 切换到所选模式"; notice_col = UI_ACCENT; }
    ui_pixel_label_set_text(s_notice, notice);
    ui_pixel_text_color(s_notice, notice_col);

    const char *action = !link || !s.have_remote ? "长按确定返回"
                       : r.running ? "上下选择 · 长按下键停止"
                                   : "按确定 启动 mihomo";
    ui_pixel_label_set_text(s_footer, action);
}

void refresh_status_bar() {
    const uint64_t now = esp_timer_get_time();
    if (s_status_updated_us && now - s_status_updated_us < 5000000) return;
    ui_pixel_battery_set(&s_battery, bsp_battery_soc());
    auto &wifi = WifiManager::GetInstance();
    int bars = 0;
    uint32_t col = UI_TEXT_DIM;
    if (wifi.IsConnected()) {
        int rssi = wifi.GetRssi();
        bars = rssi >= -60 ? 3 : (rssi >= -75 ? 2 : 1);
        col = UI_TEXT;
    }
    ui_pixel_wifi_bars_set(s_wifi_bars, bars, col);
    s_status_updated_us = now;
}

void tick(lv_timer_t *) {
    proxy_remote_t r;
    bool got = false;
    portENTER_CRITICAL(&s_inbox_mux);
    if (s_inbox_full) { r = s_inbox; s_inbox_full = false; got = true; }
    portEXIT_CRITICAL(&s_inbox_mux);
    const uint64_t now = now_ms();
    if (got) proxy_apply_remote(&s_state, &r);
    proxy_tick(&s_state, now);
    // 刚连上但还没收到状态:每 2s 主动问一次
    if (s_transport_ok && udp_audio_connected() && !s_state.have_remote &&
        now - s_last_query_ms >= 2000) {
        s_last_query_ms = now;
        send_request(PROXY_REQ_QUERY, PROXY_MODE_RULE);
    }
    if (app_registry_screen_is_on()) {
        refresh_status_bar();
        render();
    }
}

void build_ui() {
    s_screen = lv_obj_create(nullptr);
    ui_pixel_background(s_screen);
    lv_obj_t *top = ui_pixel_top_bar(s_screen);
    ui_pixel_wifi_bars_create(top, 12, 18, s_wifi_bars);
    lv_obj_t *name = ui_pixel_label(top, "代理开关", &buddy_font_16, UI_TEXT);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 42, 0);
    s_battery = ui_pixel_battery_create(top, 206, 6, UI_TEXT, &lv_font_montserrat_14);
    s_link = label(top, "", &buddy_font_16, UI_TEXT_DIM, 120, 4, 82, 20, LV_TEXT_ALIGN_RIGHT);

    // 大色块
    s_badge = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_badge);
    lv_obj_set_pos(s_badge, 40, 40);
    lv_obj_set_size(s_badge, 160, 84);
    lv_obj_set_style_bg_opa(s_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_badge, 10, 0);
    lv_obj_clear_flag(s_badge, LV_OBJ_FLAG_SCROLLABLE);
    s_badge_cn = label(s_badge, "--", &buddy_font_16, 0xFFFFFF, 0, 10, 160, 22, LV_TEXT_ALIGN_CENTER);
    s_badge_en = label(s_badge, "OFFLINE", &lv_font_montserrat_32, 0xFFFFFF, 0, 36, 160, 40,
                       LV_TEXT_ALIGN_CENTER);

    s_run = label(s_screen, "", &buddy_font_16, UI_TEXT, 12, 134, 216, 22, LV_TEXT_ALIGN_CENTER);
    s_rates = label(s_screen, "", &lv_font_montserrat_14, UI_TEXT_DIM, 12, 158, 216, 20,
                    LV_TEXT_ALIGN_CENTER);
    s_sys = label(s_screen, "", &buddy_font_16, UI_TEXT_DIM, 12, 180, 216, 22, LV_TEXT_ALIGN_CENTER);

    // 三个模式选项
    for (int i = 0; i < PROXY_MODE_COUNT; i++) {
        lv_obj_t *c = lv_obj_create(s_screen);
        lv_obj_remove_style_all(c);
        lv_obj_set_pos(c, 12 + i * 74, 210);
        lv_obj_set_size(c, 68, 32);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(c, 6, 0);
        lv_obj_set_style_border_color(c, lv_color_hex(0xFFFFFF), 0);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        label(c, proxy_mode_label((proxy_mode_t)i), &buddy_font_16, UI_TEXT, 0, 7, 68, 20,
              LV_TEXT_ALIGN_CENTER);
        s_choices[i] = c;
    }
    s_notice = label(s_screen, "", &buddy_font_16, UI_TEXT_DIM, 12, 248, 216, 22, LV_TEXT_ALIGN_CENTER);
    s_footer = ui_pixel_footer(s_screen, "", "长按确定返回", &buddy_font_16);
}

void init() {}

void start() {
    proxy_state_init(&s_state);
    portENTER_CRITICAL(&s_inbox_mux);
    s_inbox_full = false;
    portEXIT_CRITICAL(&s_inbox_mux);
    s_last_query_ms = 0;
    s_status_updated_us = 0;

    // 通道:先挂 handler 再开 socket,首条下行就不会落到语音解析
    udp_audio_set_event_handler(on_udp_event);
    s_transport_ok = udp_audio_init() == 0;
    if (!s_transport_ok) ESP_LOGE(TAG, "UDP 通道启动失败");

    if (!bsp_lvgl_lock(1000)) return;
    if (!s_screen) {
        build_ui();
        render();
        lv_screen_load(s_screen);
        s_timer = lv_timer_create(tick, 200, nullptr);
    }
    bsp_lvgl_unlock();
    ESP_LOGI(TAG, "started; transport=%d", s_transport_ok);
}

void stop() {
    // 先停定时器和界面,再关通道,最后摘 handler(deinit 已等 RX 任务退出)
    if (bsp_lvgl_lock(-1)) {
        if (s_timer) { lv_timer_delete(s_timer); s_timer = nullptr; }
        if (s_screen) { lv_obj_delete(s_screen); s_screen = nullptr; }
        s_wifi_bars[0] = s_wifi_bars[1] = s_wifi_bars[2] = nullptr;
        s_battery = {};
        bsp_lvgl_unlock();
    }
    udp_audio_deinit();
    udp_audio_set_event_handler(nullptr);
    s_transport_ok = false;
    ESP_LOGI(TAG, "stopped");
}

void on_key(uint8_t btn, app_btn_event_t event, uint16_t) {
    proxy_key_t key;
    if (event == BTN_EVT_CLICK && btn == BSP_BTN_UP) key = PROXY_KEY_UP_CLICK;
    else if (event == BTN_EVT_CLICK && btn == BSP_BTN_DOWN) key = PROXY_KEY_DOWN_CLICK;
    else if (event == BTN_EVT_CLICK && btn == BSP_BTN_OK) key = PROXY_KEY_OK_CLICK;
    else if (event == BTN_EVT_LONG && btn == BSP_BTN_DOWN) key = PROXY_KEY_DOWN_LONG;
    else return;
    if (!bsp_lvgl_lock(500)) return;
    if (s_screen) {
        proxy_mode_t mode = PROXY_MODE_RULE;
        const bool link = s_transport_ok && udp_audio_connected();
        proxy_req_t req = proxy_on_key(&s_state, key, link, now_ms(), &mode);
        if (req != PROXY_REQ_NONE) {
            ESP_LOGI(TAG, "request %d mode=%s", (int)req, proxy_mode_key(mode));
            send_request(req, mode);
        }
        render();
    }
    bsp_lvgl_unlock();
}
} // namespace

extern const passport_app_t g_proxy_app = {
    .id = APP_ID_PROXY,
    .name = "代理开关",
    .en_name = "PROXY",
    .desc = "查看和切换 Mac 上的\nmihomo 代理模式",
    .tag = "需电脑",
    .theme_color = 0x4C8DFF,
    .init = init,
    .start = start,
    .stop = stop,
    .on_key = on_key,
};
