/*
 * radar_data_export.h — 雷达数据接口合约（给算法团队）
 *
 * 这个文件是"固件团队"与"坐姿/专注/睡眠算法团队"之间的接口约定。
 * 算法团队只需要 #include 这一个头文件,所有数据入口都在这里。
 *
 * 合约版本: V1.0 (2026-07-23)
 * 维护方: 固件团队
 *
 * === 数据流全景 ===
 *
 *   R60ABD1 UART ──→ r60abd1.c (帧解析)
 *                       │
 *           ┌───────────┼───────────┐
 *           ▼           ▼           ▼
 *     ① 原始帧字节   ② 结构化快照  ③ FSM 语义事件
 *     (r60_frame_cb)  (r60_status_t)  (fsm_event_cb)
 *           │           │               │
 *           ▼           ▼               ▼
 *     算法训练数据   特征提取输入    监督学习标签
 *
 * === 使用方式 ===
 *
 * 算法团队只需三步拿到数据:
 *
 *   1. 注册帧回调,拿到每一帧的原始字节:
 *        r60_init(&parser, my_frame_callback, my_user_data);
 *
 *   2. 周期读取结构化快照(建议 200ms 一次):
 *        const r60_status_t *s = r60_status(&parser);
 *
 *   3. 注册 FSM 事件回调,拿到带标签的事件流:
 *        fsm_init(&fsm, &cfg, my_event_callback, my_user_data);
 *
 * === 数据采集模式(为 M4 阶段准备) ===
 *
 *   调用 radar_recorder_start("/sdcard/capture.bin") 后,
 *   每一帧原始字节 + 时间戳 + FSM 事件标签 会写入指定文件。
 *   默认关闭,不影响正常固件性能。
 */

#ifndef RADAR_DATA_EXPORT_H
#define RADAR_DATA_EXPORT_H

#include "r60abd1.h"
#include "radar_fsm.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * 第一部分: r60_status_t 字段语义与取值范围
 *
 * 这是算法团队做特征工程时直接读取的结构体。
 * 每个字段的语义和边界都在这里给死——算法模型不应依赖未列出的语义。
 * ================================================================ */

/*
 * r60_status_t 字段合约(由 r60abd1.h 定义,此处仅文档化):
 *
 *   presence        0=无人 1=有人
 *                   上报方式:状态变化时上报(无人→有人 <0.5s, 有人→无人 40s)
 *
 *   motion          0=无运动 1=静止 2=活动
 *                   上报方式:状态变化时上报(<0.5s)
 *
 *   body_move       体动幅度参数 0~100
 *                   上报方式:每秒一次
 *                   0~10: 微动(呼吸级)
 *                   11~30: 小幅度动作(写字、翻书)
 *                   31~60: 中幅度动作(调整坐姿)
 *                   61~100: 大幅度动作(起身、挥手)
 *
 *   distance_cm     目标距离 0~65535 cm
 *                   上报方式:每2秒一次
 *                   书桌场景:30~80cm 为正常学习距离
 *                   床头场景:50~150cm 为正常睡眠距离
 *
 *   breath_rate     呼吸率 0~35 次/分(0=无效)
 *                   上报方式:每3秒一次
 *                   儿童睡眠正常:14~20次/分(需实测标定)
 *                   睡眠区间上限(sleep_breath_max) 默认 20
 *
 *   heart_rate      心率 60~120 次/分(0=无效)
 *                   上报方式:每3秒一次
 *
 *   in_bed          0=离床 1=在床
 *                   上报方式:状态变化时上报
 *                   入床:满足能量条件后即时上报
 *                   离床:约30秒后输出离床状态
 *
 *   sleep_stage     睡眠分期(仅入床状态有效)
 *                   0=深睡 1=浅睡 2=清醒 3=无(离床/实时模式)
 *                   上报方式:入床状态下每10分钟一次
 *                   ⚠️ 与 V1.3 协议一致: 0x00深睡 0x01浅睡 0x02清醒
 *
 *   last_update_ms  最近一次任意上报的时间戳(宿主毫秒时钟)
 *                   用于判断数据新鲜度
 */

/* 数据新鲜度检查:超过此毫秒数未更新则视为 stale */
#define RADAR_DATA_STALE_MS  5000

static inline bool radar_data_is_fresh(const r60_status_t *s, uint32_t now_ms) {
    return (now_ms - s->last_update_ms) < RADAR_DATA_STALE_MS;
}

/* ================================================================
 * 第二部分: FSM 事件作为监督学习标签
 *
 * FSM 输出的事件是经过防抖确认的"真值事件"。
 * 算法团队可以做:
 *   - 用 EV_SEATED / EV_AWAY 作为在离位分类器的 ground truth
 *   - 用 EV_ALARM_STOP 时刻前后 3 秒的 body_move 序列训练坐起检测模型
 *   - 用 EV_ALARM_RESUME 时刻前后 3 分钟的 breath_rate 序列训练回睡检测模型
 * ================================================================ */

/*
 * fsm_event_t 事件标签合约(由 radar_fsm.h 定义,此处仅文档化):
 *
 *   EV_ALARM_STOP        闹钟判停:连续 judge_stop_ms(默认3000ms) 体动≥wake_move_th
 *                        或离床。确认后的标签,不是瞬时触发器。
 *
 *   EV_ALARM_RESUME      回睡检测:守护期内 在床+静止+呼吸回落 持续 resleep_ms(默认180s)
 *                        确认躺回后的标签。
 *
 *   EV_WAKE_CONFIRMED    起床确认:离床≥leave_confirm_ms(默认60s) 或守护期满
 *                        真正的"已起床"标签。
 *
 *   EV_SEATED            入座:体动>5 或 presence=1 持续 seat_debounce_ms(默认5s)
 *                        去抖确认后的标签。
 *
 *   EV_AWAY              离座:不满足入座条件持续 seat_debounce_ms
 *
 *   EV_STATE_CHANGED     任意状态迁移。data 参数为新 fsm_state_t 值。
 */

/* 事件标签导出:把 fsm_event_t 转成人类可读字符串 */
static inline const char *fsm_event_name(fsm_event_t ev) {
    switch (ev) {
    case EV_ALARM_STOP:     return "alarm_stop";
    case EV_ALARM_RESUME:   return "alarm_resume";
    case EV_WAKE_CONFIRMED: return "wake_confirmed";
    case EV_SEATED:         return "seated";
    case EV_AWAY:           return "away";
    case EV_STATE_CHANGED:  return "state_changed";
    default:                return "unknown";
    }
}

static inline const char *fsm_state_name(fsm_state_t st) {
    switch (st) {
    case FSM_SLEEP_MONITOR: return "sleep_monitor";
    case FSM_ALARM_RINGING: return "alarm_ringing";
    case FSM_GUARD_PERIOD:  return "guard_period";
    case FSM_DAY_AWAY:      return "day_away";
    case FSM_DAY_SEATED:    return "day_seated";
    default:                return "unknown";
    }
}

/* ================================================================
 * 第三部分: 原始帧回调(已内置,r60abd1.h 的 r60_frame_cb)
 *
 * 每一帧原始字节在 r60abd1.c 解析后被回调。
 * 算法团队可以直接在这里拿到:
 *   - ctl/cmd: 帧类型(对应协议的控制字/命令字)
 *   - data/len: 原始数据区(大端字节序)
 *   - 时间戳: 通过 r60_status(&parser)->last_update_ms 获取
 *
 * 要记录原始帧,只需在回调里把 ctl/cmd/len/data 写入文件或发送到采集服务。
 * 注意:在高帧率模式下(~20fps),每秒约 400 字节,一小时约 1.4MB。
 * ================================================================ */

/* ================================================================
 * 第四部分: 数据采集模式(可选,默认关闭)
 *
 * 为 M4 阶段"儿童雷达数据采集"准备。
 * 开启后将所有原始帧+时间戳+FSM事件标签写入二进制文件。
 * ================================================================ */

/* 采集文件中的一条记录(二进制格式,便于流式写入和解析) */
typedef struct __attribute__((packed)) {
    uint32_t timestamp_ms;   /* 宿主毫秒时钟 */
    uint8_t  ctl;            /* 控制字(帧大类) */
    uint8_t  cmd;            /* 命令字(帧子类) */
    uint16_t data_len;       /* 数据长度 */
    uint8_t  fsm_state;      /* 当前FSM状态 */
    uint8_t  fsm_event;      /* 如果本tick有事件则非0xFF,值为 fsm_event_t */
    /* data[data_len] 紧随其后 */
} radar_record_header_t;

/* 启动/停止原始帧记录。
 * filepath: 绝对路径(如 "/spiffs/capture_20260723.bin")或 NULL 停止。
 * 返回 true 表示成功打开文件并开始记录。
 * 注意:记录期间 Flash 写入频繁,建议仅在数据采集阶段使用。 */
bool radar_recorder_start(const char *filepath);
void radar_recorder_stop(void);
bool radar_recorder_is_active(void);

/* 注册一个额外的帧监听器(算法团队可以在这里挂自己的处理逻辑)。
 * 与 r60_frame_cb 的区别: recorder 回调在 FSM tick 之后触发,
 * 因此 record 中包含了 fsm_state 标签。
 * 允许多个监听器同时注册(链表),互不干扰。
 * 返回 >=0 的 listener_id,失败返回 -1。 */
typedef void (*radar_record_listener_cb)(const radar_record_header_t *hdr,
                                          const uint8_t *data, void *user);
int  radar_recorder_add_listener(radar_record_listener_cb cb, void *user);
void radar_recorder_remove_listener(int listener_id);

#ifdef __cplusplus
}
#endif
#endif /* RADAR_DATA_EXPORT_H */
