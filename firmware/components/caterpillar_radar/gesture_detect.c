/*
 * gesture_detect.c — 人体动作持续监测实现（纯 C，仿 radar_fsm 风格）
 *
 * 每 tick 调用 gd_update()，内部维护滑动窗口 + 去抖状态机。
 * 动作变化时通过回调通知，也可随时 gd_get_state() 查询。
 */

#include "gesture_detect.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define ELAPSED(now, since) ((uint32_t)((now) - (since)))

void gd_default_config(gd_config_t *c)
{
    c->window_samples        = 16;    /* ~1.6s @ 10Hz */
    c->wave_x_amp_min_mm     = 80;   /* 8cm 振幅 */
    c->wave_speed_cross_min  = 2;     /* x过零或speed翻转≥2次 */
    c->wave_debounce_in      = 2;      /* 连续 1 帧确认 = ~100ms */
    c->wave_hold_ms          = 3000;   /* 停止挥手后保持 3 秒 */
}

void gd_init(gd_t *g, const gd_config_t *cfg, gd_event_cb cb, void *user)
{
    memset(g, 0, sizeof(*g));
    if (cfg) g->cfg = *cfg;
    else     gd_default_config(&g->cfg);
    g->cb      = cb;
    g->cb_user = user;
    g->samples = (gd_sample_t *)calloc(g->cfg.window_samples, sizeof(gd_sample_t));
    g->state.gesture = GD_NONE;
}

void gd_deinit(gd_t *g)
{
    if (g->samples) { free(g->samples); g->samples = NULL; }
    g->sample_count = 0;
}

void gd_feed_ld2450(gd_t *g, bool valid, int16_t x_mm, int16_t y_mm, int16_t speed_cms)
{
    g->last.ld_valid    = valid;
    g->last.ld_x_mm     = x_mm;
    g->last.ld_y_mm     = y_mm;
    g->last.ld_speed_cms = speed_cms;
}

const gd_state_t* gd_get_state(const gd_t *g) { return &g->state; }

const char* gd_gesture_name(gd_gesture_t g)
{
    switch (g) {
    case GD_NONE:    return "none";
    case GD_WAVE:    return "wave";
    case GD_SQUAT:   return "squat";
    case GD_UNKNOWN: return "unknown";
    default:         return "?";
    }
}

/* ---- 挥手检测（基于滑动窗口） ---- */
static bool detect_wave(const gd_t *g, char *detail, size_t detail_len)
{
    int16_t x_min = 32767, x_max = -32768;
    uint8_t speed_cross = 0, x_cross = 0;
    int16_t last_speed = 0, last_x = 0;
    bool first = true;

    /* 先算 x 均值作为中心线 */
    int32_t x_sum = 0;
    uint16_t valid_n = 0;
    for (uint16_t i = 0; i < g->sample_count; i++) {
        if (g->samples[i].ld_valid) { x_sum += g->samples[i].ld_x_mm; valid_n++; }
    }
    if (valid_n < 4) return false;
    int16_t x_center = (int16_t)(x_sum / valid_n);

    for (uint16_t i = 0; i < g->sample_count; i++) {
        if (!g->samples[i].ld_valid) continue;

        int16_t x  = g->samples[i].ld_x_mm;
        int16_t sp = g->samples[i].ld_speed_cms;

        if (x < x_min) x_min = x;
        if (x > x_max) x_max = x;

        /* x 坐标穿越中心线 */
        if (!first && ((last_x - x_center) * (x - x_center) < 0)) x_cross++;

        /* speed 符号翻转 */
        if (!first && ((last_speed > 0 && sp < 0) || (last_speed < 0 && sp > 0)))
            speed_cross++;

        first = false;
        last_x = x;
        last_speed = sp;
    }

    int16_t x_amp = x_max - x_min;
    bool amp_ok  = (x_amp >= g->cfg.wave_x_amp_min_mm);
    bool cross_ok = (speed_cross >= g->cfg.wave_speed_cross_min) ||
                    (x_cross >= g->cfg.wave_speed_cross_min);
    /* 满足 x 振幅+穿越，或单独满足 speed 翻转次数 */
    if ((amp_ok && cross_ok) || speed_cross >= g->cfg.wave_speed_cross_min) {
        snprintf(detail, detail_len, "x_amp=%dmm sc=%d xc=%d", (int)x_amp, (int)speed_cross, (int)x_cross);
        return true;
    }
    return false;
}

/* ---- 状态迁移（带去抖） ---- */
static void set_state(gd_t *g, gd_gesture_t gesture, const char *detail)
{
    if (g->state.gesture == gesture) return;
    g->state.gesture      = gesture;
    g->state.detected_at_ms = g->now_ms;
    g->state.held_ms        = 0;
    snprintf(g->state.detail, sizeof(g->state.detail), "%s", detail ? detail : "");
    if (g->cb) g->cb(gesture, g->state.detail, g->cb_user);

    /* 重置去抖状态 */
    g->wave_in_count = 0;
    g->wave_last_seen_ms = 0;
}

void gd_update(gd_t *g, uint32_t now_ms)
{
    g->now_ms = now_ms;

    /* 推入滑动窗口 */
    if (g->sample_count < g->cfg.window_samples) {
        g->samples[g->sample_count++] = g->last;
    } else {
        /* 环形覆盖 */
        g->samples[g->sample_head] = g->last;
        g->sample_head = (g->sample_head + 1) % g->cfg.window_samples;
    }

    if (g->sample_count < 4) return;  /* 数据太少，不分析 */

    /* 更新已持续时间 */
    if (g->state.gesture != GD_NONE) {
        g->state.held_ms = ELAPSED(now_ms, g->state.detected_at_ms);
    }

    /* 挥手检测 + 基于时间的去抖 */
    {
        char detail[64];
        bool waving = detect_wave(g, detail, sizeof(detail));

        if (waving) {
            g->wave_last_seen_ms = now_ms;
            if (g->wave_in_count < 255) g->wave_in_count++;
        } else {
            g->wave_in_count = 0;
        }

        if (g->state.gesture == GD_NONE && g->wave_in_count >= g->cfg.wave_debounce_in) {
            set_state(g, GD_WAVE, detail);
        } else if (g->state.gesture == GD_WAVE &&
                   ELAPSED(now_ms, g->wave_last_seen_ms) >= g->cfg.wave_hold_ms) {
            set_state(g, GD_NONE, "wave ended");
        }
    }

    /* 蹲下检测已迁移至事件驱动方案（caterpillar_radar_glue.cc） */
}
