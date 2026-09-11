/* broadcast_sched.c — 播报触发调度实现（纯 C） */
#include "broadcast_sched.h"
#include <string.h>

void bc_init(bc_sched_t *b, const bc_config_t *cfg, bc_play_fn play, void *user)
{
    memset(b, 0, sizeof(*b));
    b->cfg = *cfg;
    b->play = play;
    b->user = user;
}

static void try_play(bc_sched_t *b, bc_slot_t slot)
{
    if (b->played[slot])
        return;
    if (b->play && b->play(slot, b->user)) {
        b->played[slot] = true;
        b->pending[slot] = false;
    } else {
        b->pending[slot] = true;   /* 稍后 bc_tick 重试 */
    }
}

void bc_on_wake_confirmed(bc_sched_t *b)
{
    try_play(b, SLOT_MORNING);
}

void bc_on_seated(bc_sched_t *b, uint16_t now_min)
{
    // 原型期不限制窗口，入座即播；量产恢复时间判断
    try_play(b, SLOT_EVENING);
}

void bc_tick(bc_sched_t *b, uint16_t now_min, uint8_t day_of_month)
{
    /* 跨天复位 */
    if (day_of_month != b->day_of_month) {
        b->day_of_month = day_of_month;
        memset(b->played, 0, sizeof(b->played));
        memset(b->pending, 0, sizeof(b->pending));
    }
    /* 晨间兜底：deadline 到了还没播 → 强制播 */
    if (!b->played[SLOT_MORNING] && now_min >= b->cfg.morning_deadline_min &&
        now_min < b->cfg.morning_deadline_min + 30)
        try_play(b, SLOT_MORNING);
    /* pending 重试 */
    for (int s = 0; s < SLOT_COUNT; s++) {
        if (b->pending[s] && !b->played[s]) {
            if (s == SLOT_EVENING &&
                (now_min < b->cfg.evening_start_min || now_min >= b->cfg.evening_end_min))
                continue;      /* 晚间窗口外不再重试 */
            try_play(b, (bc_slot_t)s);
        }
    }
}
