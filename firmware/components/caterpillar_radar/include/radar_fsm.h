/*
 * radar_fsm.h — 毛毛虫智感核心状态机（产品定义 F1/F2 的端侧实现，原型）
 *
 * 纯 C、tick 驱动、无平台依赖：宿主周期性（建议 200ms）调用 radar_fsm_tick()，
 * 传入雷达状态快照与当前毫秒时钟；状态迁移经事件回调抛出，由宿主接闹钟、
 * 播报调度（broadcast_sched）、表情/舵机与云端上报。
 *
 * 夜间闹钟链路（《产品定义》F2 状态机）：
 *   SLEEP_MONITOR --宿主到达唤醒窗+浅睡--> ALARM_RINGING（宿主开始响铃）
 *   ALARM_RINGING --坐起/大幅体动>=judge_stop_ms 或离床--> GUARD_PERIOD（EV_ALARM_STOP）
 *   GUARD_PERIOD  --躺回+呼吸回落+静止>=resleep_ms--> ALARM_RINGING（EV_ALARM_RESUME）
 *   GUARD_PERIOD  --离床>=leave_confirm_ms--> DAY_AWAY（EV_WAKE_CONFIRMED → 触发晨间播报）
 *   GUARD_PERIOD  --守护期满仍在床且清醒--> DAY_AWAY（EV_WAKE_CONFIRMED, 保守放行）
 *
 * 白天链路（F1 简化版）：
 *   DAY_AWAY <--> DAY_SEATED（EV_SEATED / EV_AWAY，带去抖）
 */
#ifndef RADAR_FSM_H
#define RADAR_FSM_H

#include "r60abd1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FSM_SLEEP_MONITOR = 0,      /* 夜间睡眠监测（雷达固件已完成，宿主仅接收状态） */    
    FSM_ALARM_RINGING,          /* 夜间闹钟响铃中（宿主响铃+播报） */
    FSM_GUARD_PERIOD,           /* 夜间守护期（宿主播报） */
    FSM_DAY_AWAY,               /* 白天离座 */
    FSM_DAY_SEATED,             /* 白天入座 */
} fsm_state_t;

typedef enum {
    EV_ALARM_STOP = 0,     /* 判停：检测到清醒动作，宿主停止响铃 */
    EV_ALARM_RESUME,       /* 回睡：宿主重新响铃（音量从上次级别续增） */
    EV_WAKE_CONFIRMED,     /* 起床确认：触发晨间播报 + 上报云端 */
    EV_SEATED,             /* 入座（白天） */
    EV_AWAY,               /* 离座（白天） */
    EV_STATE_CHANGED,      /* 任意状态迁移（data=新状态），供表情/日志 */
    EV_SQUAT,              /* 蹲下确认 */
    EV_SQUAT_END,          /* 蹲下结束 */
} fsm_event_t;

typedef void (*fsm_event_cb)(fsm_event_t ev, int data, void *user);

typedef struct {
    /* 判停：响铃中体动幅度 >= wake_move_th 持续 judge_stop_ms，或离床，判定醒来 */
    uint8_t  wake_move_th;        /* 默认 30 (0~100) */
    uint32_t judge_stop_ms;       /* 默认 3000 —— "持续坐起≥3秒"级确认，防翻身误判 */
    /* 回睡：守护期内 在床 且体动 < still_th 且呼吸率在睡眠区间 持续 resleep_ms */
    uint8_t  still_th;            /* 默认 10 */
    uint8_t  sleep_breath_max;    /* 默认 20 次/分（儿童睡眠呼吸上限，需实测标定） */
    uint32_t resleep_ms;          /* 默认 180000 (3 分钟) */
    /* 起床确认：守护期内离床持续 leave_confirm_ms 未回床 */
    uint32_t leave_confirm_ms;    /* 默认 60000 */
    uint32_t guard_period_ms;     /* 守护期总长，默认 1200000 (20 分钟) */
    /* 白天入离座去抖 */
    uint32_t seat_debounce_ms;    /* 默认 5000 */
} fsm_config_t;

typedef struct {
    fsm_state_t  state;
    fsm_config_t cfg;
    fsm_event_cb cb;
    void        *cb_user;
    /* 内部计时锚点（宿主时钟 ms） */
    uint32_t t_state_enter;
    uint32_t t_move_start;      /* 判停：连续大体动起点，0=未开始 */
    uint32_t t_still_start;     /* 回睡：连续睡眠特征起点 */
    uint32_t t_leave_start;     /* 起床确认：离床起点 */
    uint32_t t_seat_change;     /* 白天：在/离位变化起点 */
} radar_fsm_t;

void fsm_default_config(fsm_config_t *cfg);
void fsm_init(radar_fsm_t *f, const fsm_config_t *cfg, fsm_event_cb cb, void *user);

/* 宿主控制：进入夜间睡眠监测 / 唤醒窗口到达开始响铃 / 切白天模式 */
void fsm_enter_sleep_monitor(radar_fsm_t *f, uint32_t now_ms);
void fsm_start_alarm(radar_fsm_t *f, uint32_t now_ms);
void fsm_enter_day(radar_fsm_t *f, uint32_t now_ms);

/* 周期驱动（建议 200ms 一次） */
void fsm_tick(radar_fsm_t *f, const r60_status_t *s, uint32_t now_ms);

static inline fsm_state_t fsm_state(const radar_fsm_t *f) { return f->state; }

/*====================================================================
 * 蹲下检测状态机（事件驱动，独立于 radar_fsm_t）
 *====================================================================*/

typedef struct {
    uint8_t  squat_move_min;      /* body_move 阈值，默认 40 */
    uint16_t squat_xy_max_cm;     /* x/y 波动上限 cm，默认 15 */
    uint8_t  squat_high_count_min;/* 连续超阈值最少次数，默认 2 */
    uint32_t squat_hold_ms;       /* 保持时间 ms，默认 3000 */
} squat_config_t;

typedef struct {
    squat_config_t cfg;
    bool           active;
    uint8_t        high_count;
    uint32_t       last_high_ms;
    int16_t        pos_x_prev, pos_y_prev;
    bool           pos_stable;
    bool           pos_initialized;
    fsm_event_cb   cb;
    void          *cb_user;
} squat_fsm_t;

void squat_default_config(squat_config_t *c);
void squat_init(squat_fsm_t *f, const squat_config_t *cfg,
                fsm_event_cb cb, void *user);
void squat_on_body_move(squat_fsm_t *f, uint8_t body_move, uint32_t now);
void squat_on_position(squat_fsm_t *f, int16_t x_cm, int16_t y_cm, uint32_t now);
static inline bool squat_is_active(const squat_fsm_t *f) { return f->active; }

#ifdef __cplusplus
}
#endif
#endif /* RADAR_FSM_H */
