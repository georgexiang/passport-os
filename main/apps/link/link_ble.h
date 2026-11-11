// main/apps/link/link_ble.h —— 和 Mac(Passport Voice)之间的蓝牙(BLE)通道,
// 「离席哨」「Orca 看板」共用(两个应用不会同时运行),和 Wi-Fi UDP 通道二选一。
//
// 设备是外设(peripheral),Mac 上的 Passport Voice 是中心(central)。一个 GATT 服务、两个特征:
//   服务  6a1f0001-3c55-4d2b-9a7e-5f3b2c1d0e00  广播名 "Passport-Link"
//   RX    6a1f0002-…  Mac 写入(write / write without response):下行 JSON 行
//   TX    6a1f0003-…  设备通知(notify):上行 JSON 行
// 消息内容和 Wi-Fi 版完全相同(每个应用自己的 JSON 行),以 '\n' 结尾;一行可以拆成多次写入/通知,
// 收方按 '\n' 拼回整行。只在应用运行且选了蓝牙模式时开启,退出即关闭并释放内存。
// 用哪个通道记在 NVS("link"/"ble"),各应用共用同一个选择。
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*link_ble_line_cb)(const char *line, size_t len);

// 启动 NimBLE 并开始广播。收到的每一整行交给 cb(在 NimBLE 主机任务里调用,须非阻塞)。
bool link_ble_start(link_ble_line_cb cb);
// 断开、停止广播、关掉 NimBLE 并释放内存。幂等。
void link_ble_stop(void);
// 已连接且 Mac 订阅了通知
bool link_ble_connected(void);
// 发一行(应含结尾 '\n'),按 MTU 拆成多次通知。未连接时丢弃并返回 false。
bool link_ble_send(const char *line, size_t len);

// 通道选择(NVS 持久化,所有应用共用)
bool link_ble_mode(void);
void link_ble_set_mode(bool ble);

#ifdef __cplusplus
}
#endif
