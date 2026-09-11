/*
 * broadcast_sched.h — 播报触发调度（产品定义 F4 的端侧触发层，原型）
 *
 * 职责：接收 radar_fsm 事件 + 宿主时钟，按触发规则决定"现在播哪个包"。
 * 播放与下载不在本组件内——宿主注册 play/download 回调
 *（StackChan 上播放走 xiaozhi 的音频通道，下载走 esp_http_client，见 INTEGRATION.md）。
 *
 * 触发规则（与 manifest.json 对应）：
 *   morning: EV_WAKE_CONFIRMED 触发；到 morning_deadline 仍未播则强制播（宿主 tick 传时钟）
 *   evening: 晚间窗口内首次 EV_SEATED 触发；窗口结束仍未播则放弃（次日并入）
 *   每个包每天只播一次
 */
#ifndef BROADCAST_SCHED_H
#define BROADCAST_SCHED_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { SLOT_MORNING = 0, SLOT_EVENING, SLOT_COUNT } bc_slot_t;

/* 宿主实现：播放某个包（返回 false 表示当前不可播，如正在对话，稍后重试） */
typedef bool (*bc_play_fn)(bc_slot_t slot, void *user);

typedef struct {
    /* 当日窗口（从 manifest 解析后由宿主写入，分钟数 = 小时*60+分） */
    uint16_t morning_deadline_min;   /* 例 7*60+25 */
    uint16_t evening_start_min;      /* 例 20*60+30 */
    uint16_t evening_end_min;        /* 例 21*60+15 */
} bc_config_t;

typedef struct {
    bc_config_t cfg;
    bc_play_fn  play;
    void       *user;
    bool        played[SLOT_COUNT];
    bool        pending[SLOT_COUNT]; /* 已触发但 play 返回 false，等待重试 */
    uint8_t     day_of_month;        /* 跨天自动复位 */
} bc_sched_t;

void bc_init(bc_sched_t *b, const bc_config_t *cfg, bc_play_fn play, void *user);

/* 事件入口：radar_fsm 的回调里转发进来 */
void bc_on_wake_confirmed(bc_sched_t *b);
void bc_on_seated(bc_sched_t *b, uint16_t now_min);

/* 周期 tick（分钟级即可）：处理 deadline 强制播、pending 重试、跨天复位 */
void bc_tick(bc_sched_t *b, uint16_t now_min, uint8_t day_of_month);

#ifdef __cplusplus
}
#endif
#endif /* BROADCAST_SCHED_H */
