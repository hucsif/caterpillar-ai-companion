/* radar_fsm.c — 毛毛虫智感核心状态机实现（纯 C） */
#include "radar_fsm.h"
#include <string.h>

#define ELAPSED(now, since) ((uint32_t)((now) - (since)))

void fsm_default_config(fsm_config_t *c)
{
    c->wake_move_th     = 30;
    c->judge_stop_ms    = 3000;
    c->still_th         = 10;
    c->sleep_breath_max = 20;
    c->resleep_ms       = 180000;
    c->leave_confirm_ms = 60000;
    c->guard_period_ms  = 1200000;
    c->seat_debounce_ms = 5000;
}

static void set_state(radar_fsm_t *f, fsm_state_t st, uint32_t now)
{
    if (f->state == st)
        return;
    f->state = st;
    f->t_state_enter = now;
    f->t_move_start = f->t_still_start = f->t_leave_start = f->t_seat_change = 0;
    if (f->cb)
        f->cb(EV_STATE_CHANGED, (int)st, f->cb_user);
}

static void emit(radar_fsm_t *f, fsm_event_t ev)
{
    if (f->cb)
        f->cb(ev, 0, f->cb_user);
}

void fsm_init(radar_fsm_t *f, const fsm_config_t *cfg, fsm_event_cb cb, void *user)
{
    memset(f, 0, sizeof(*f));
    if (cfg) f->cfg = *cfg;
    else     fsm_default_config(&f->cfg);
    f->cb = cb;
    f->cb_user = user;
    f->state = FSM_DAY_AWAY;
}

void fsm_enter_sleep_monitor(radar_fsm_t *f, uint32_t now) { set_state(f, FSM_SLEEP_MONITOR, now); }
void fsm_start_alarm(radar_fsm_t *f, uint32_t now)         { set_state(f, FSM_ALARM_RINGING, now); }
void fsm_enter_day(radar_fsm_t *f, uint32_t now)           { set_state(f, FSM_DAY_AWAY, now); }

/* ---- 各状态 tick 处理 ---- */

static void tick_alarm_ringing(radar_fsm_t *f, const r60_status_t *s, uint32_t now)
{
    const fsm_config_t *c = &f->cfg;

    /* 离床 = 立即判停（人已经起来走了） */
    if (!s->in_bed || !s->presence) {
        emit(f, EV_ALARM_STOP);
        set_state(f, FSM_GUARD_PERIOD, now);
        f->t_leave_start = now;   /* 已离床，直接开始计离床确认 */
        return;
    }
    /* 坐起/举手 = 大幅体动持续 judge_stop_ms（防翻身瞬时误判） */
    if (s->body_move >= c->wake_move_th) {
        if (f->t_move_start == 0)
            f->t_move_start = now;
        if (ELAPSED(now, f->t_move_start) >= c->judge_stop_ms) {
            emit(f, EV_ALARM_STOP);
            set_state(f, FSM_GUARD_PERIOD, now);
        }
    } else {
        f->t_move_start = 0;      /* 体动中断，重新累计 */
    }
}

static void tick_guard(radar_fsm_t *f, const r60_status_t *s, uint32_t now)
{
    const fsm_config_t *c = &f->cfg;

    /* 路径 A：离床确认 → 起床 */
    if (!s->in_bed || !s->presence) {
        if (f->t_leave_start == 0)
            f->t_leave_start = now;
        f->t_still_start = 0;
        if (ELAPSED(now, f->t_leave_start) >= c->leave_confirm_ms) {
            emit(f, EV_WAKE_CONFIRMED);
            set_state(f, FSM_DAY_AWAY, now);
        }
        return;
    }
    f->t_leave_start = 0;         /* 回床了，离床计时清零 */

    /* 路径 B：回睡检测 → 再次响铃 */
    bool sleep_like = (s->body_move < c->still_th) &&
                      (s->breath_rate > 0) &&
                      (s->breath_rate <= c->sleep_breath_max);
    if (sleep_like) {
        if (f->t_still_start == 0)
            f->t_still_start = now;
        if (ELAPSED(now, f->t_still_start) >= c->resleep_ms) {
            emit(f, EV_ALARM_RESUME);
            set_state(f, FSM_ALARM_RINGING, now);
            return;
        }
    } else {
        f->t_still_start = 0;
    }

    /* 路径 C：守护期满，人在床但清醒（如坐床上看书）→ 保守放行 */
    if (ELAPSED(now, f->t_state_enter) >= c->guard_period_ms) {
        emit(f, EV_WAKE_CONFIRMED);
        set_state(f, FSM_DAY_AWAY, now);
    }
}

static void tick_day(radar_fsm_t *f, const r60_status_t *s, uint32_t now)
{
    const fsm_config_t *c = &f->cfg;
    // 原型V0：presence 在某些雷达固件中不工作，改用体动>5 作为"有人"判断
    bool seated_now = (s->body_move > 5) || s->presence;
    bool want_seated = (f->state == FSM_DAY_SEATED);

    if (seated_now != want_seated) {
        if (f->t_seat_change == 0)
            f->t_seat_change = now;
        if (ELAPSED(now, f->t_seat_change) >= c->seat_debounce_ms) {
            if (seated_now) {
                set_state(f, FSM_DAY_SEATED, now);
                emit(f, EV_SEATED);
            } else {
                set_state(f, FSM_DAY_AWAY, now);
                emit(f, EV_AWAY);
            }
        }
    } else {
        f->t_seat_change = 0;
    }
}

void fsm_tick(radar_fsm_t *f, const r60_status_t *s, uint32_t now)
{
    switch (f->state) {
    case FSM_SLEEP_MONITOR:
        /* 睡眠监测本身由雷达固件完成；响铃时机由宿主(闹钟窗口+浅睡判断)调用
         * fsm_start_alarm() 触发，这里无自迁移。 */
        break;
    case FSM_ALARM_RINGING:
        tick_alarm_ringing(f, s, now);
        break;
    case FSM_GUARD_PERIOD:
        tick_guard(f, s, now);
        break;
    case FSM_DAY_AWAY:
    case FSM_DAY_SEATED:
        tick_day(f, s, now);
        break;
    default:
        break;
    }
}

/*====================================================================
 * 蹲下检测状态机（事件驱动，独立于 radar_fsm_t）
 *====================================================================*/

void squat_default_config(squat_config_t *c)
{
    c->squat_move_min       = 40;
    c->squat_xy_max_cm      = 15;
    c->squat_high_count_min = 2;
    c->squat_hold_ms        = 3000;
}

void squat_init(squat_fsm_t *f, const squat_config_t *cfg,
                fsm_event_cb cb, void *user)
{
    memset(f, 0, sizeof(*f));
    if (cfg) f->cfg = *cfg;
    else     squat_default_config(&f->cfg);
    f->cb = cb;
    f->cb_user = user;
}

void squat_on_body_move(squat_fsm_t *f, uint8_t body_move, uint32_t now)
{
    const squat_config_t *c = &f->cfg;

    if (body_move >= c->squat_move_min) {
        f->high_count++;
        f->last_high_ms = now;

        if (!f->active && f->high_count >= c->squat_high_count_min && f->pos_stable) {
            f->active = true;
            if (f->cb) f->cb(EV_SQUAT, (int)body_move, f->cb_user);
        }
    } else {
        f->high_count = 0;
    }

    if (f->active && ELAPSED(now, f->last_high_ms) >= c->squat_hold_ms) {
        f->active = false;
        if (f->cb) f->cb(EV_SQUAT_END, 0, f->cb_user);
    }
}

void squat_on_position(squat_fsm_t *f, int16_t x_cm, int16_t y_cm, uint32_t now)
{
    const squat_config_t *c = &f->cfg;

    if (!f->pos_initialized) {
        f->pos_x_prev = x_cm;
        f->pos_y_prev = y_cm;
        f->pos_initialized = true;
        f->pos_stable = true;
        return;
    }

    int16_t dx = x_cm - f->pos_x_prev;
    int16_t dy = y_cm - f->pos_y_prev;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;

    f->pos_stable = (dx < (int16_t)c->squat_xy_max_cm &&
                     dy < (int16_t)c->squat_xy_max_cm);
    f->pos_x_prev = x_cm;
    f->pos_y_prev = y_cm;
}
