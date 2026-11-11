// main/apps/orca/orca_core.h —— Orca agent 看板的纯逻辑(无 ESP-IDF 依赖,可在宿主机测试)。
//
// Mac 端汇总 Orca(本机或已配对的远程服务器)的 agent 状态,经语音应用同一个
// UDP 33333 通道下发(两个应用不会同时运行)。单个 UDP 事件包 ≤ 512B,放不下全部
// agent,所以一份快照可以拆成几包,每包是一行 JSON:
//
//   {"type":"orca.snapshot","source":"arm-ubuntu","total":13,"seq":42,"part":0,"parts":3,
//    "agents":[{"t":"claude","s":"waiting","r":"qm-product","b":"dev",
//               "k":"允许设置 SANDBOX…","tool":"Bash","age":95}, ...]}
//
//   seq   同一份快照的各包相同,每份快照递增
//   part  从 0 开始的包序号;parts = 总包数(缺省 1,即单包快照)
//   t = agent 类型   s = working | waiting | blocked | done | idle
//   r = 仓库  b = 分支  k = 任务(已截短)  tool = 当前工具  age = 进入该状态的秒数
//
// Mac 端按"需要关注 → 工作中 → 完成"排好序;设备最多保留 ORCA_MAX_AGENTS 个。
// 各包必须按 part 顺序到达,缺了任何一包这份快照就作废,等下一份(每 5s 一份)。
// 设备 ORCA_STALE_MS 收不到完整快照就判为"没有数据"。
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ORCA_MAX_AGENTS 20
#define ORCA_MAX_PARTS  8
#define ORCA_STALE_MS   15000u   // Mac 每 5s 重发一次;15s 没收到就显示"没有数据"

typedef enum {
    ORCA_ST_WORKING = 0,
    ORCA_ST_WAITING,   // 等人处理(权限请求 / 提问)
    ORCA_ST_BLOCKED,   // 卡住
    ORCA_ST_DONE,
    ORCA_ST_IDLE,
    ORCA_ST_UNKNOWN,
} orca_state_t;

typedef struct {
    char type[16];
    char repo[24];
    char branch[24];
    char task[64];
    char tool[16];
    uint8_t state;      // orca_state_t
    uint32_t age_s;     // 收到时已在该状态的秒数
} orca_agent_t;

typedef struct {
    char source[24];            // "本机" 或远程服务器名
    uint16_t total;             // Orca 里的 agent 总数(可能多于收到的条数)
    uint8_t count;              // agents[] 有效条数
    orca_agent_t agents[ORCA_MAX_AGENTS];
    uint64_t received_ms;       // 0 = 从未收到完整快照
    // 以下两项只在拼包用的暂存板上有意义
    uint32_t seq;
    uint8_t next_part;          // 期待的下一包序号
} orca_board_t;

typedef enum {
    ORCA_PART_NOT_SNAPSHOT = 0, // 不是 orca.snapshot(同通道上的其他消息),静默忽略
    ORCA_PART_BAD,              // 是快照但格式错误
    ORCA_PART_DROPPED,          // 乱序或缺包,这份快照作废
    ORCA_PART_PENDING,          // 收下了,还在等后续包
    ORCA_PART_COMPLETE,         // 最后一包收齐,staging 就是完整快照
} orca_part_result_t;

// 把一包合并进暂存板 staging。返回 COMPLETE 时 staging 即一份完整快照(received_ms 已设)。
orca_part_result_t orca_parse_part(const char *json, size_t len, uint64_t now_ms,
                                   orca_board_t *staging);

// 单包便利函数:清空 out 后解析;只有一包就收齐时返回 true。
bool orca_parse_snapshot(const char *json, size_t len, uint64_t now_ms, orca_board_t *out);

// 数据是否新鲜(收到过且未超过 ORCA_STALE_MS)。
bool orca_board_fresh(const orca_board_t *b, uint64_t now_ms);

// 需要关注(waiting / blocked)的条数。
uint8_t orca_attention_count(const orca_board_t *b);
uint8_t orca_count_state(const orca_board_t *b, orca_state_t st);

// 新快照里出现了上一份没有的"需要关注"项 → 应该提醒(响铃 + 亮屏)。
// 按 type+repo+branch 识别同一个 agent;prev 为空(从未收到)时只要有关注项就提醒。
bool orca_should_alert(const orca_board_t *prev, const orca_board_t *next);

// 状态的中文短名,如 "等你处理"。
const char *orca_state_label(uint8_t st);

// "5秒" / "3分钟" / "2小时" / "4天",写入 buf。
void orca_format_age(uint32_t seconds, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
