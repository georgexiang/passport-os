// main/apps/orca/app_orca.cc —— 「Orca 看板」:显示 Orca agent dashboard 的状态(只读)。
//
// 数据由 Mac 端汇总后经 UDP 33333 下发(协议见 orca_core.h),复用语音应用的
// udp_audio 通道:进入本应用时接管下行 EVENT 的解析,退出时交还。两个应用不会同时运行。
//
// 界面:
//   列表页  上/下 选择,确定 进详情;每行 = 状态色点 + agent 类型 + 仓库 + 状态
//   详情页  类型、状态、已持续、仓库/分支、当前工具、任务;确定 返回列表
//   长按下键 = 切换 Wi-Fi / 蓝牙 通道(和「离席哨」共用同一个选择)
//   长按确定 = 整机统一的"返回"
// 有新的 agent 进入"等你处理/卡住"时:亮屏 + 系统提醒音一次。
#include "apps/app_base.h"
#include "apps/orca/orca_core.h"
#include "apps/voice/udp_audio.h"
#include "apps/link/link_ble.h"
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
#include <atomic>
#include <cstdio>
#include <cstring>

namespace {
constexpr const char *kTag = "orca";
constexpr int kRows = 6;           // 列表一页的行数
constexpr int kRowY = 74;
constexpr int kRowH = 30;

// 三块快照缓冲(各约 3KB)只在应用运行时分配,退出即释放,不常驻内存:
//   s_staging  RX 任务拼包用(只有 RX 任务访问)
//   s_inbox    拼好的完整快照,RX 任务写、界面定时器读,用互斥锁保护
//   s_board    当前显示的数据(只在 LVGL 锁内访问)
// 用 FreeRTOS 互斥锁而不是关中断的 portMUX:3KB 拷贝不该在关中断时做。
struct Boards { orca_board_t staging, inbox, board; };
Boards *s_mem = nullptr;
SemaphoreHandle_t s_inbox_lock = nullptr;
StaticSemaphore_t s_inbox_lock_buf;
bool s_inbox_new = false;
#define s_board (s_mem->board)

uint8_t s_sel = 0;
bool s_detail = false;
bool s_sound_pending = false;
bool s_transport_up = false;
bool s_ble = false;         // 当前通道:false = Wi-Fi UDP,true = 蓝牙

lv_obj_t *s_screen;
lv_timer_t *s_timer;
lv_obj_t *s_wifi_bars[3];
ui_pixel_battery_t s_battery{};
uint64_t s_battery_updated_us;
lv_obj_t *s_summary, *s_source, *s_empty, *s_footer, *s_mode;
lv_obj_t *s_row_bg[kRows], *s_row_dot[kRows], *s_row_name[kRows], *s_row_state[kRows];
lv_obj_t *s_d_title, *s_d_state, *s_d_place, *s_d_tool, *s_d_task;

uint64_t now_ms() { return static_cast<uint64_t>(esp_timer_get_time() / 1000); }

// ---- 通道 ----
void on_event_line(const char *json, size_t len) {
    // 在 udp RX 任务里:拼包到 staging,收齐一份再拷进收件箱。
    // Passport Voice 每秒还会发 pc.status / mihomo.state,解析器直接忽略。
    // 整个处理都持锁:stop() 释放缓冲前也要拿这把锁,即使 udp_audio_deinit()
    // 等不到 RX 任务退出,这里也不会碰到已释放的内存。
    if (!s_inbox_lock || xSemaphoreTake(s_inbox_lock, pdMS_TO_TICKS(50)) != pdTRUE) return;
    Boards *m = s_mem;
    orca_part_result_t r = m ? orca_parse_part(json, len, now_ms(), &m->staging) : ORCA_PART_NOT_SNAPSHOT;
    if (r == ORCA_PART_COMPLETE) {
        m->inbox = m->staging;
        s_inbox_new = true;
    }
    xSemaphoreGive(s_inbox_lock);
    switch (r) {
    case ORCA_PART_BAD:
        ESP_LOGW(kTag, "bad snapshot part: %.*s", (int)(len > 60 ? 60 : len), json);
        break;
    case ORCA_PART_DROPPED:
        ESP_LOGW(kTag, "snapshot part out of order, waiting for the next one");
        break;
    default:
        break;
    }
}

// 告诉 Mac 端:Orca 看板在前台,请下发快照(Mac 端据此只在需要时轮询 Orca)。
void send_hello() {
    static const char kLine[] = "{\"event\":\"orca.hello\"}\n";
    if (s_ble) link_ble_send(kLine, sizeof(kLine) - 1);
    else udp_audio_notify_event(kLine, sizeof(kLine) - 1);
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
        udp_audio_deinit();                       // 先关 socket 再撤处理函数
        udp_audio_set_event_handler(nullptr);
    }
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

uint32_t state_color(uint8_t st) {
    switch (st) {
    case ORCA_ST_WAITING: return UI_DANGER;
    case ORCA_ST_BLOCKED: return UI_WARNING;
    case ORCA_ST_WORKING: return UI_SUCCESS;
    default: return UI_TEXT_DIM;
    }
}

// ---- 渲染 ----
void render_status_bar(uint64_t now_us) {
    if (s_mode) {   // 通道标签:蓝牙模式下 Wi-Fi 信号格没有意义
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

void render() {
    if (!s_screen || !s_mem) return;
    const uint64_t now = now_ms();
    const bool fresh = orca_board_fresh(&s_board, now);
    const bool have = fresh && s_board.count > 0;
    if (s_sel >= s_board.count) s_sel = s_board.count ? s_board.count - 1 : 0;
    if (!have) s_detail = false;

    // 顶部汇总
    char sum[64];
    if (!s_transport_up) {
        std::snprintf(sum, sizeof(sum), s_ble ? "蓝牙启动失败" : "网络未就绪");
    } else if (!fresh) {
        std::snprintf(sum, sizeof(sum), "等待电脑转发");
    } else {
        const uint8_t need = orca_attention_count(&s_board);
        const uint8_t work = orca_count_state(&s_board, ORCA_ST_WORKING);
        if (need) std::snprintf(sum, sizeof(sum), "等你 %u · 工作中 %u", need, work);
        else if (work) std::snprintf(sum, sizeof(sum), "工作中 %u · 共 %u 个", work, s_board.total);
        else std::snprintf(sum, sizeof(sum), "全部空闲 · 共 %u 个", s_board.total);
    }
    ui_pixel_label_set_text(s_summary, sum);
    ui_pixel_text_color(s_summary, fresh && orca_attention_count(&s_board) ? UI_DANGER : UI_TEXT);
    // 来源 + 位置(选中第几个 / 共收到几个),收到的少于 Orca 总数时提示
    char src[64];
    const char *name = fresh && s_board.source[0] ? s_board.source : "Orca agent 看板";
    if (have && s_board.count < s_board.total)
        std::snprintf(src, sizeof(src), "%s  %u/%u（显示前 %u）", name, s_sel + 1, s_board.count, s_board.count);
    else if (have)
        std::snprintf(src, sizeof(src), "%s  %u/%u", name, s_sel + 1, s_board.count);
    else
        std::snprintf(src, sizeof(src), "%s", name);
    ui_pixel_label_set_text(s_source, src);

    // 空状态
    hide(s_empty, have);
    if (!have) {
        ui_pixel_label_set_text(s_empty,
            !s_transport_up ? (s_ble ? "长按下键切换到 Wi-Fi" : "请先连接 Wi-Fi\n或长按下键改用蓝牙") :
            !fresh ? (s_ble ? "蓝牙广播中\n等待 Passport Voice 连接" : "在电脑上打开 Passport Voice") :
            "现在没有 agent");
    }

    // 列表(按页显示,选中行高亮)
    const int page = s_sel / kRows;
    for (int i = 0; i < kRows; i++) {
        const int idx = page * kRows + i;
        const bool show = have && !s_detail && idx < s_board.count;
        hide(s_row_bg[i], !show); hide(s_row_dot[i], !show);
        hide(s_row_name[i], !show); hide(s_row_state[i], !show);
        if (!show) continue;
        const orca_agent_t &a = s_board.agents[idx];
        char name[64];
        std::snprintf(name, sizeof(name), "%s  %s", a.type, a.repo);
        ui_pixel_label_set_text(s_row_name[i], name);
        ui_pixel_label_set_text(s_row_state[i], orca_state_label(a.state));
        ui_pixel_bg_color(s_row_dot[i], state_color(a.state));
        ui_pixel_text_color(s_row_state[i], state_color(a.state));
        ui_pixel_bg_color(s_row_bg[i], idx == s_sel ? UI_SURFACE_ALT : UI_BG);
    }

    // 详情
    const bool detail = have && s_detail;
    for (lv_obj_t *o : {s_d_title, s_d_state, s_d_place, s_d_tool, s_d_task}) hide(o, !detail);
    if (detail) {
        const orca_agent_t &a = s_board.agents[s_sel];
        ui_pixel_label_set_text(s_d_title, a.type);
        char line[96], age[16];
        orca_format_age(a.age_s + static_cast<uint32_t>((now - s_board.received_ms) / 1000), age, sizeof(age));
        std::snprintf(line, sizeof(line), "%s · %s", orca_state_label(a.state), age);
        ui_pixel_label_set_text(s_d_state, line);
        ui_pixel_text_color(s_d_state, state_color(a.state));
        std::snprintf(line, sizeof(line), "%s%s%s", a.repo, a.branch[0] ? " · " : "", a.branch);
        ui_pixel_label_set_text(s_d_place, line);
        std::snprintf(line, sizeof(line), a.tool[0] ? "正在用 %s" : "", a.tool);
        ui_pixel_label_set_text(s_d_tool, line);
        ui_pixel_label_set_text(s_d_task, a.task[0] ? a.task : "（没有任务描述）");
    }

    ui_pixel_label_set_text(s_footer, detail ? "确定返回列表" : have ? "上下选择 · 确定看详情" :
        s_ble ? "长按下键:换 Wi-Fi" : "长按下键:换蓝牙");
    render_status_bar(esp_timer_get_time());
}

void tick(lv_timer_t *) {
    if (!s_mem) return;
    // 取走收件箱里的新快照(直接拷进 s_board,不在栈上放 3KB 的临时副本)
    bool got = false;
    if (s_inbox_new && xSemaphoreTake(s_inbox_lock, 0) == pdTRUE) {
        if (s_inbox_new) {
            const bool alert = orca_should_alert(&s_board, &s_mem->inbox);
            s_board = s_mem->inbox;
            s_inbox_new = false;
            got = true;
            if (alert) {
                s_sound_pending = true;
                app_registry_wake_for_notice();   // 调用方已持有 LVGL 锁(定时器在 LVGL 任务里)
                ESP_LOGI(kTag, "agent needs attention");
            }
        }
        xSemaphoreGive(s_inbox_lock);
    }
    if (got) app_registry_keep_awake();
    // 提醒音分段播放:一直调用到它报告完成(与番茄钟相同的约定)
    if (s_sound_pending) {
        if (orca_attention_count(&s_board) == 0) { ui_sfx_cancel_alarm(); s_sound_pending = false; }
        else if (app_registry_sound_notice()) s_sound_pending = false;
    }
    // 每 5 秒向电脑报到一次,电脑据此知道看板在前台
    static uint64_t last_hello;
    const uint64_t now = now_ms();
    if (s_transport_up && now - last_hello >= 5000) { send_hello(); last_hello = now; }
    if (app_registry_screen_is_on() || got) render();
}

// ---- 生命周期 ----
void init() {}

void start() {
    if (!s_inbox_lock) s_inbox_lock = xSemaphoreCreateMutexStatic(&s_inbox_lock_buf);
    if (!s_mem) {
        s_mem = static_cast<Boards *>(heap_caps_calloc(1, sizeof(Boards), MALLOC_CAP_8BIT));
        if (!s_mem) { ESP_LOGE(kTag, "no memory for %u-byte board", (unsigned)sizeof(Boards)); return; }
    }
    s_inbox_new = false;
    // 通道:先挂处理函数再开 socket(udp_audio 的约定)
    s_ble = link_ble_mode();
    s_transport_up = transport_open();
    if (!s_transport_up) ESP_LOGE(kTag, "%s channel unavailable", s_ble ? "BLE" : "UDP");
    if (!bsp_lvgl_lock(1000)) return;
    if (s_screen) { bsp_lvgl_unlock(); return; }
    s_sel = 0; s_detail = false; s_sound_pending = false;

    s_screen = lv_obj_create(nullptr);
    ui_pixel_background(s_screen);
    lv_obj_t *top = ui_pixel_top_bar(s_screen);
    ui_pixel_wifi_bars_create(top, 12, 18, s_wifi_bars);
    lv_obj_t *name = ui_pixel_label(top, "Orca 看板", &buddy_font_16, UI_TEXT);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 42, 0);
    s_mode = ui_pixel_label(top, "", &buddy_font_16, UI_TEXT_DIM);
    lv_obj_align(s_mode, LV_ALIGN_LEFT_MID, 128, 0);
    s_battery = ui_pixel_battery_create(top, 206, 6, UI_TEXT, &lv_font_montserrat_14);

    s_summary = label("", &ui_font_20, UI_TEXT, 12, 34, 216, 26);
    s_source = label("", &buddy_font_16, UI_TEXT_DIM, 12, 58, 216, 18);
    s_empty = label("", &buddy_font_16, UI_TEXT_DIM, 12, 130, 216, 60);
    lv_label_set_long_mode(s_empty, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_empty, LV_TEXT_ALIGN_CENTER, 0);

    for (int i = 0; i < kRows; i++) {
        const int y = kRowY + 6 + i * kRowH;
        s_row_bg[i] = box(8, y - 3, 224, kRowH - 2, UI_BG);
        s_row_dot[i] = box(14, y + 7, 8, 8, UI_TEXT_DIM);
        s_row_name[i] = label("", &buddy_font_16, UI_TEXT, 28, y, 132, 22);
        s_row_state[i] = label("", &buddy_font_16, UI_TEXT_DIM, 160, y, 72, 22);
        lv_obj_set_style_text_align(s_row_state[i], LV_TEXT_ALIGN_RIGHT, 0);
    }

    s_d_title = label("", &ui_font_20, UI_TEXT, 12, 86, 216, 28);
    s_d_state = label("", &buddy_font_16, UI_TEXT, 12, 118, 216, 22);
    s_d_place = label("", &buddy_font_16, UI_TEXT_DIM, 12, 144, 216, 22);
    s_d_tool = label("", &buddy_font_16, UI_WARNING, 12, 170, 216, 22);
    s_d_task = label("", &buddy_font_16, UI_TEXT, 12, 198, 216, 66);
    lv_label_set_long_mode(s_d_task, LV_LABEL_LONG_WRAP);

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
    // RX 任务已停、处理函数已撤,定时器已删,此时没有人再访问快照缓冲。
    // 拿一下锁,确保最后一次 on_event_line 拷贝已经结束,再释放。
    if (s_inbox_lock && xSemaphoreTake(s_inbox_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        Boards *m = s_mem;
        s_mem = nullptr;
        s_inbox_new = false;
        xSemaphoreGive(s_inbox_lock);
        heap_caps_free(m);
    }
}

// 长按下键:换通道。先关旧的(释放内存),再开新的;拼了一半的快照作废。
void switch_transport() {
    transport_close();
    s_ble = !s_ble;
    link_ble_set_mode(s_ble);
    if (s_inbox_lock && xSemaphoreTake(s_inbox_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_mem) s_mem->staging.next_part = 0xFF;
        xSemaphoreGive(s_inbox_lock);
    }
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
        const bool have = orca_board_fresh(&s_board, now_ms()) && s_board.count > 0;
        if (have) {
            if (btn == BSP_BTN_OK) s_detail = !s_detail;
            else if (!s_detail && btn == BSP_BTN_UP) s_sel = s_sel ? s_sel - 1 : s_board.count - 1;
            else if (!s_detail && btn == BSP_BTN_DOWN) s_sel = (s_sel + 1) % s_board.count;
        }
        // 按任何键都算"看到了",停止还没响完的提醒音
        if (s_sound_pending) { ui_sfx_cancel_alarm(); s_sound_pending = false; }
        render();
    }
    bsp_lvgl_unlock();
}
} // namespace

extern const passport_app_t g_orca_app = {
    .id = APP_ID_ORCA,
    .name = "Orca 看板",
    .en_name = "ORCA",
    .desc = "Orca agent 状态\n需电脑转发",
    .tag = "需电脑",
    .theme_color = 0x5B8DEF,
    .init = init,
    .start = start,
    .stop = stop,
    .on_key = on_key,
};
