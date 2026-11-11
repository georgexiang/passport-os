// main/apps/proxy/proxy_core.h —— 「代理开关」纯逻辑:mihomo 状态模型与按键状态机。
// 不依赖 ESP-IDF / LVGL,主机测试直接编译(tests/test_proxy_core.c)。
// 协议见上级目录 mihomo-控制-设计.md:Mac 端(Passport Voice)发 mihomo.state,
// 设备发 mihomo.set_mode / mihomo.start / mihomo.stop / mihomo.query。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PROXY_MODE_RULE = 0,
    PROXY_MODE_GLOBAL,
    PROXY_MODE_DIRECT,
    PROXY_MODE_COUNT,
    PROXY_MODE_UNKNOWN = PROXY_MODE_COUNT,
} proxy_mode_t;

typedef enum {
    PROXY_SYS_OFF = 0,
    PROXY_SYS_ON,
    PROXY_SYS_MISMATCH,   // 开着,但端口和 mihomo 不一致
    PROXY_SYS_BROKEN,     // 开着,但 mihomo 未运行 → 会断网
} proxy_sys_t;

// Mac 发来的 mihomo 实际状态
typedef struct {
    bool running;
    proxy_mode_t mode;
    uint16_t port;
    proxy_sys_t sysproxy;
    uint32_t up;          // 字节/秒
    uint32_t down;
    bool busy;
    char error[64];
} proxy_remote_t;

// 设备端状态
typedef struct {
    bool have_remote;      // 收到过至少一次 mihomo.state
    proxy_remote_t remote;
    proxy_mode_t selected; // UP/DOWN 选中的模式
    bool pending;          // 已发请求,等 Mac 回 mihomo.state
    uint64_t pending_since_ms;
    bool confirm_stop;     // 停止前等待二次确认
    uint64_t confirm_since_ms;
} proxy_state_t;

// 设备要发给 Mac 的请求
typedef enum {
    PROXY_REQ_NONE = 0,
    PROXY_REQ_SET_MODE,
    PROXY_REQ_START,
    PROXY_REQ_STOP,
    PROXY_REQ_QUERY,
} proxy_req_t;

typedef enum {
    PROXY_KEY_UP_CLICK = 0,
    PROXY_KEY_DOWN_CLICK,
    PROXY_KEY_OK_CLICK,
    PROXY_KEY_DOWN_LONG,
} proxy_key_t;

#define PROXY_PENDING_TIMEOUT_MS  12000u  // 启动最多要等 10s,留余量
#define PROXY_CONFIRM_TIMEOUT_MS  4000u

void proxy_state_init(proxy_state_t *s);

// 解析 Mac 发来的一行 JSON。不是 mihomo.state 或格式错误返回 false。
bool proxy_parse_state(const char *json, size_t len, proxy_remote_t *out);

// 收到 Mac 状态:更新显示、清除"等待中"。首次收到时把选中项对齐当前模式。
void proxy_apply_remote(proxy_state_t *s, const proxy_remote_t *r);

// 按键。link_up=false 时一律忽略。返回要发的请求,*mode_out 只在 SET_MODE 时有效。
proxy_req_t proxy_on_key(proxy_state_t *s, proxy_key_t key, bool link_up,
                         uint64_t now_ms, proxy_mode_t *mode_out);

// 每 100ms 调用:处理等待超时、确认超时。
void proxy_tick(proxy_state_t *s, uint64_t now_ms);

// 序列化设备 → Mac 的请求(含结尾 '\n')。返回字节数,失败 0。
size_t proxy_build_request(char *buf, size_t cap, proxy_req_t req, proxy_mode_t mode);

const char *proxy_mode_key(proxy_mode_t m);    // "rule" / "global" / "direct" / ""
const char *proxy_mode_label(proxy_mode_t m);  // "规则" / "全局" / "直连" / "--"
const char *proxy_mode_en(proxy_mode_t m);     // "RULE" / "GLOBAL" / "DIRECT" / "--"

// 速度格式化:"512 B/s" / "1.2 KB/s" / "3.4 MB/s"
void proxy_format_rate(uint32_t bps, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
