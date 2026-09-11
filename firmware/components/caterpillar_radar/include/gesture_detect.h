/*
 * gesture_detect.h — 人体动作持续监测模块（纯 C，仿 radar_fsm 风格）
 *
 * 用法：
 *   1. gd_init() 初始化
 *   2. 每 tick 调用 gd_feed_ld2450() / gd_feed_r60() / gd_update()
 *   3. 任何时候 gd_get_state() 查询当前动作状态
 *
 * 状态变化时通过回调通知，可设置 debounce 避免误触发。
 */

#ifndef GESTURE_DETECT_H
#define GESTURE_DETECT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 可识别的动作类型 ---- */
typedef enum {
    GD_NONE = 0,        /* 无动作 */
    GD_WAVE,            /* 挥手 */
    GD_SQUAT,           /* 蹲下（需DP5 z轴有效） */
    GD_UNKNOWN,         /* 有动作但无法归类 */
    GD_COUNT
} gd_gesture_t;

/* ---- 动作状态快照 ---- */
typedef struct {
    gd_gesture_t gesture;       /* 当前动作 */
    uint32_t     detected_at_ms;/* 检测到的时间 */
    uint32_t     held_ms;       /* 已持续多久 */
    char         detail[64];    /* 调试信息（如 "x_amp=320mm cross=6"） */
} gd_state_t;

/* ---- 状态变化回调 ---- */
typedef void (*gd_event_cb)(gd_gesture_t gesture, const char *detail, void *user);

/* ---- 每帧采样数据 ---- */
typedef struct {
    bool     ld_valid;
    int16_t  ld_x_mm;
    int16_t  ld_y_mm;
    int16_t  ld_speed_cms;
    /* R60ABD1 */
    int16_t  r60_z_cm;
    uint8_t  r60_body_move;
    uint16_t r60_distance_cm;
} gd_sample_t;

/* ---- 可调参数 ---- */
typedef struct {
    /* 连续检测窗口 */
    uint16_t window_samples;        /* 滑动窗口帧数（默认 16） */

    /* 挥手 */
    int16_t  wave_x_amp_min_mm;     /* x 振幅阈值 mm（默认 200） */
    uint8_t  wave_speed_cross_min;  /* speed 符号翻转最少次数（默认 4） */
    uint8_t  wave_debounce_in;      /* 连续检测到 N 次才确认（默认 2） */
    uint32_t wave_hold_ms;          /* 停止后保持状态 ms（默认 3000） */

    /* 蹲下检测已迁移至事件驱动方案（caterpillar_radar_glue.cc） */
} gd_config_t;

/* ---- 实例 ---- */
typedef struct {
    gd_config_t cfg;
    gd_event_cb cb;
    void       *cb_user;

    /* 环形缓冲 */
    gd_sample_t *samples;
    uint16_t     sample_count;
    uint16_t     sample_head;       /* 写入位置 */

    /* 当前状态 */
    gd_state_t   state;

    /* 去抖 */
    uint8_t      wave_in_count;
    uint32_t     wave_last_seen_ms;   /* 最后一次检测到挥手的时间 */
    uint32_t     wave_left_at_ms;     /* 退出挥手的时间（冷却用） */

    /* 上一帧数据（挥手检测用） */
    gd_sample_t  last;

    /* 外部时钟 */
    uint32_t     now_ms;
} gd_t;

/* ---- API ---- */
void gd_default_config(gd_config_t *c);
void gd_init(gd_t *g, const gd_config_t *cfg, gd_event_cb cb, void *user);
void gd_deinit(gd_t *g);

/* 每 tick 喂入最新数据（顺序：先喂后 update） */
void gd_feed_ld2450(gd_t *g, bool valid, int16_t x_mm, int16_t y_mm, int16_t speed_cms);
/* gd_feed_r60 已移除——蹲下检测迁移至事件驱动 */

/* 每 tick 调用一次：推进滑动窗口 + 检测 + 去抖 + 状态迁移 */
void gd_update(gd_t *g, uint32_t now_ms);

/* 查询当前动作状态 */
const gd_state_t* gd_get_state(const gd_t *g);

/* 获取当前动作名称字符串 */
const char* gd_gesture_name(gd_gesture_t g);

#ifdef __cplusplus
}
#endif
#endif /* GESTURE_DETECT_H */
