// main/apps/away/away_core.h —— 「离席哨」的纯逻辑(无 ESP-IDF 依赖,可在宿主机测试)。
//
// 你离开电脑时,Mac 端 Passport Voice 每 5 分钟读一次飞书(含静音群)的未读数,
// 经 UDP 33333 下发到本应用;有新增就亮屏 + 响铃。两端都可以切换"离开/回来"。
//
// 下行(Mac → 设备),每行一个 JSON:
//   {"type":"away.state","away":true,"since":1731230000}         离开状态(每秒随 pc.status 一起发)
//   {"type":"away.unread","seq":7,"part":0,"parts":2,"total":3,
//    "checked":1731230300,
//    "items":[{"n":"项目沟通群","c":2,"m":1,"s":"飞书"}, ...]}
//     n = 群名(已截短)  c = 未读条数  m = 1 静音群 / 0 普通  s = 来源(哪个飞书/账户)
//     一份快照可以拆成几包(单包 ≤ 512B),各包 seq 相同、part 从 0 递增;缺包就作废。
//     checked = Mac 完成这次查询的时间(epoch 秒,0 = 还没查过)
// 上行(设备 → Mac):
//   {"event":"away.hello"}                 应用在前台,每 5s 一次
//   {"event":"away.set","away":true}       在设备上切换离开/回来
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AWAY_MAX_ITEMS 16
#define AWAY_MAX_PARTS 6

typedef struct {
    char name[48];
    char source[16];
    uint16_t count;
    bool muted;
} away_item_t;

typedef struct {
    uint8_t count;              // items[] 有效条数
    uint16_t total;             // 有未读的群总数(可能多于 items)
    uint32_t checked;           // Mac 最近一次查询时间(epoch 秒)
    away_item_t items[AWAY_MAX_ITEMS];
    uint64_t received_ms;       // 0 = 从未收到完整快照
    uint32_t seq;               // 拼包暂存用
    uint8_t next_part;
} away_board_t;

typedef enum {
    AWAY_MSG_NONE = 0,     // 不是本应用的消息
    AWAY_MSG_BAD,          // 是本应用的消息但格式不对
    AWAY_MSG_STATE,        // away.state 已解析到 *away / *since
    AWAY_MSG_PENDING,      // away.unread 的一包已收,等后续包
    AWAY_MSG_DROPPED,      // 乱序或缺包,本份作废
    AWAY_MSG_COMPLETE,     // away.unread 一份收齐
} away_msg_t;

// 解析一行下行事件。away.state 写入 *away / *since;away.unread 拼包到 *staging。
away_msg_t away_parse(const char *json, size_t len, uint64_t now_ms,
                      away_board_t *staging, bool *away, uint32_t *since);

// 未读总条数
uint32_t away_unread_total(const away_board_t *b);

// 新快照里有没有"新增":出现新群,或某个群条数变多。用于决定是否响铃。
bool away_should_alert(const away_board_t *prev, const away_board_t *next);

// "23 分钟" / "1 小时 5 分" / "刚刚"
void away_format_duration(uint32_t seconds, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
