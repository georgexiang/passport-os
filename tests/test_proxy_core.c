#include "apps/proxy/proxy_core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static proxy_remote_t parse(const char *json) {
    proxy_remote_t r;
    assert(proxy_parse_state(json, strlen(json), &r));
    return r;
}

int main(void) {
    proxy_remote_t r;
    /* 解析:完整状态 */
    r = parse("{\"type\":\"mihomo.state\",\"running\":true,\"mode\":\"global\",\"port\":7891,"
              "\"sysproxy\":\"mismatch\",\"up\":2048,\"down\":5,\"busy\":false,\"error\":\"\"}\n");
    assert(r.running && r.mode == PROXY_MODE_GLOBAL && r.port == 7891);
    assert(r.sysproxy == PROXY_SYS_MISMATCH && r.up == 2048 && r.down == 5 && !r.busy);
    /* 未运行、未知模式、超长错误被截断 */
    r = parse("{\"type\":\"mihomo.state\",\"running\":false,\"mode\":\"\",\"sysproxy\":\"broken\","
              "\"error\":\"0123456789012345678901234567890123456789012345678901234567890123456789\"}");
    assert(!r.running && r.mode == PROXY_MODE_UNKNOWN && r.sysproxy == PROXY_SYS_BROKEN);
    assert(strlen(r.error) == sizeof(r.error) - 1);
    /* 拒绝:别的消息类型、缺 running、坏 JSON、负数/超大端口 */
    assert(!proxy_parse_state("{\"type\":\"pc.status\"}", 20, &r));
    assert(!proxy_parse_state("{\"type\":\"mihomo.state\"}", 23, &r));
    assert(!proxy_parse_state("{nope", 5, &r));
    r = parse("{\"type\":\"mihomo.state\",\"running\":true,\"port\":99999,\"up\":-5}");
    assert(r.port == 0 && r.up == 0);

    proxy_state_t s;
    proxy_mode_t m = PROXY_MODE_UNKNOWN;
    proxy_state_init(&s);
    /* 没收到状态 / 断链:按键无效 */
    assert(proxy_on_key(&s, PROXY_KEY_DOWN_LONG, true, 0, &m) == PROXY_REQ_NONE);
    r = parse("{\"type\":\"mihomo.state\",\"running\":true,\"mode\":\"direct\"}");
    proxy_apply_remote(&s, &r);
    assert(s.selected == PROXY_MODE_DIRECT);   /* 首次收到 → 选中当前模式 */
    assert(proxy_on_key(&s, PROXY_KEY_DOWN_CLICK, false, 0, &m) == PROXY_REQ_NONE);
    assert(s.selected == PROXY_MODE_DIRECT);
    /* 选择循环:direct → rule(下)→ direct(上)→ global(上) */
    proxy_on_key(&s, PROXY_KEY_DOWN_CLICK, true, 0, &m); assert(s.selected == PROXY_MODE_RULE);
    proxy_on_key(&s, PROXY_KEY_UP_CLICK, true, 0, &m);   assert(s.selected == PROXY_MODE_DIRECT);
    /* 选中与当前相同:OK 无动作 */
    assert(proxy_on_key(&s, PROXY_KEY_OK_CLICK, true, 0, &m) == PROXY_REQ_NONE);
    proxy_on_key(&s, PROXY_KEY_UP_CLICK, true, 0, &m);   assert(s.selected == PROXY_MODE_GLOBAL);
    assert(proxy_on_key(&s, PROXY_KEY_OK_CLICK, true, 100, &m) == PROXY_REQ_SET_MODE);
    assert(m == PROXY_MODE_GLOBAL && s.pending);
    /* 等待中不接受新请求 */
    assert(proxy_on_key(&s, PROXY_KEY_DOWN_CLICK, true, 200, &m) == PROXY_REQ_NONE);
    assert(s.selected == PROXY_MODE_GLOBAL);
    /* Mac 回执 → 解除等待 */
    r = parse("{\"type\":\"mihomo.state\",\"running\":true,\"mode\":\"global\"}");
    proxy_apply_remote(&s, &r);
    assert(!s.pending && s.remote.mode == PROXY_MODE_GLOBAL);
    /* 网页上改了模式 → 选中项跟上 */
    r = parse("{\"type\":\"mihomo.state\",\"running\":true,\"mode\":\"rule\"}");
    proxy_apply_remote(&s, &r);
    assert(s.selected == PROXY_MODE_RULE);
    /* 停止要二次确认;其他键取消 */
    assert(proxy_on_key(&s, PROXY_KEY_DOWN_LONG, true, 1000, &m) == PROXY_REQ_NONE && s.confirm_stop);
    assert(proxy_on_key(&s, PROXY_KEY_DOWN_CLICK, true, 1100, &m) == PROXY_REQ_NONE && !s.confirm_stop);
    assert(s.selected == PROXY_MODE_RULE);      /* 取消键不移动选中项 */
    proxy_on_key(&s, PROXY_KEY_DOWN_LONG, true, 2000, &m);
    proxy_tick(&s, 2000 + PROXY_CONFIRM_TIMEOUT_MS);   /* 确认超时 */
    assert(!s.confirm_stop);
    proxy_on_key(&s, PROXY_KEY_DOWN_LONG, true, 9000, &m);
    assert(proxy_on_key(&s, PROXY_KEY_OK_CLICK, true, 9100, &m) == PROXY_REQ_STOP && s.pending);
    /* busy 期间仍等待;结果到达后解除 */
    r = parse("{\"type\":\"mihomo.state\",\"running\":true,\"mode\":\"rule\",\"busy\":true}");
    proxy_apply_remote(&s, &r);
    assert(s.pending);
    r = parse("{\"type\":\"mihomo.state\",\"running\":false,\"mode\":\"\"}");
    proxy_apply_remote(&s, &r);
    assert(!s.pending && !s.remote.running);
    /* 未运行:确定或长按下键 = 启动(无需确认);等待中不重复发送 */
    assert(proxy_on_key(&s, PROXY_KEY_OK_CLICK, true, 9000, &m) == PROXY_REQ_START);
    assert(proxy_on_key(&s, PROXY_KEY_DOWN_LONG, true, 9500, &m) == PROXY_REQ_NONE);
    proxy_tick(&s, 9000 + PROXY_PENDING_TIMEOUT_MS);
    assert(proxy_on_key(&s, PROXY_KEY_DOWN_LONG, true, 10000, &m) == PROXY_REQ_START);
    /* 等待超时自动解除 */
    proxy_tick(&s, 10000 + PROXY_PENDING_TIMEOUT_MS - 1); assert(s.pending);
    proxy_tick(&s, 10000 + PROXY_PENDING_TIMEOUT_MS);     assert(!s.pending);

    /* 序列化 */
    char buf[96];
    size_t n = proxy_build_request(buf, sizeof(buf), PROXY_REQ_SET_MODE, PROXY_MODE_DIRECT);
    assert(n && !strcmp(buf, "{\"event\":\"mihomo.set_mode\",\"mode\":\"direct\"}\n"));
    assert(proxy_build_request(buf, sizeof(buf), PROXY_REQ_SET_MODE, PROXY_MODE_UNKNOWN) == 0);
    assert(proxy_build_request(buf, 8, PROXY_REQ_QUERY, PROXY_MODE_RULE) == 0); /* 太小 */
    n = proxy_build_request(buf, sizeof(buf), PROXY_REQ_STOP, PROXY_MODE_RULE);
    assert(n && !strcmp(buf, "{\"event\":\"mihomo.stop\"}\n"));

    proxy_format_rate(512, buf, sizeof(buf));      assert(!strcmp(buf, "512 B/s"));
    proxy_format_rate(1229, buf, sizeof(buf));     assert(!strcmp(buf, "1.2 KB/s"));
    proxy_format_rate(3565159, buf, sizeof(buf));  assert(!strcmp(buf, "3.4 MB/s"));
    assert(!strcmp(proxy_mode_label(PROXY_MODE_UNKNOWN), "--"));
    puts("proxy_core: all tests passed");
    return 0;
}
