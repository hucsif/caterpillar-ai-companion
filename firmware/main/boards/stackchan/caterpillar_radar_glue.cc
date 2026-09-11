/*
 * caterpillar_radar_glue.cc — 雷达 UART 读取 → 帧解析 → 状态机 → 播报调度
 *
 * 接线: R60ABD1 TX → G18, RX → G17, 5V → 5V, GND → GND (CoreS3 Grove PORT.C)
 * UART2, 115200 8N1
 */

#include "caterpillar_radar_glue.h"

#include <cstdio>

#include <esp_log.h>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "config.h"
#include "application.h"
#include "mcp_server.h"
#include "r60abd1.h"
#include "radar_fsm.h"
#include "broadcast_sched.h"
#include "caterpillar_http.h"

static const char* TAG = "caterpillar_radar";

/* ---- 全局实例 ---- */
static r60_parser_t   g_parser;
static radar_fsm_t    g_fsm;
static bc_sched_t     g_bc;

/* ---- 获取当前时间（分钟数和日期）用于播报调度 ---- */
static void get_time_info(uint16_t& out_minutes, uint8_t& out_day) {
    time_t now = time(nullptr);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    out_minutes = (uint16_t)(tm_info.tm_hour * 60 + tm_info.tm_min);
    out_day = (uint8_t)tm_info.tm_mday;
}

/* ---- 播报回调：bc_sched 触发 → 调 HTTP 下载播放 ---- */
static bool on_broadcast_play(bc_slot_t slot, void* /*user*/) {
    const char* type = (slot == SLOT_MORNING) ? "morning" : "evening";
    ESP_LOGI(TAG, "Broadcast trigger: %s", type);
    return caterpillar_http_play(type, nullptr);
}

/* ---- 雷达帧回调：每收到一帧就打印当前数据 ---- */
static void on_radar_frame(uint8_t ctl, uint8_t cmd,
                           const uint8_t* data, uint16_t len, void* /*user*/) {
    const r60_status_t* s = r60_status(&g_parser);
    // 根据帧类型打印对应的数据
    switch (ctl) {
    case 0x80:  // 人体存在类
        if (cmd == 0x01)      // 存在变化
            ESP_LOGI(TAG, "RADAR ▶ 有人/无人: %d | 体动:%d 距离:%dcm",
                     s->presence, s->body_move, s->distance_cm);
        else if (cmd == 0x03) // 体动幅度(1s)
            ESP_LOGI(TAG, "RADAR ▶ 体动:%d 在床:%d 睡眠期:%d",
                     s->body_move, s->in_bed, s->sleep_stage);
        // 距离(2s) 和 方位(2s) 太频繁，静默
        break;
    case 0x81:  // 呼吸类
        ESP_LOGI(TAG, "RADAR ▶ 呼吸:%d次/分", s->breath_rate);
        break;
    case 0x84:  // 睡眠类
        if (cmd == 0x01)      // 入床/离床变化
            ESP_LOGI(TAG, "RADAR ▶ 在床:%d", s->in_bed);
        else if (cmd == 0x02) // 睡眠状态(10min)
            ESP_LOGI(TAG, "RADAR ▶ 睡眠期:%d(0深/1浅/2醒)", s->sleep_stage);
        else if (cmd == 0x0F) // 模式切换回复
            ESP_LOGI(TAG, "RADAR ▶ 模式切换回复: 0x%02x", data[0]);
        break;
    case 0x85:  // 心率类
        ESP_LOGI(TAG, "RADAR ▶ 心率:%d次/分", s->heart_rate);
        break;
    default:
        break;
    }
}

/* ---- 状态机事件回调 ---- */
static void on_fsm_event(fsm_event_t ev, int data, void* /*user*/) {
    switch (ev) {
    case EV_ALARM_STOP:
        ESP_LOGI(TAG, "=== 闹钟判停：坐起确认 ===");
        break;

    case EV_ALARM_RESUME:
        ESP_LOGI(TAG, "=== 回睡再响 ===");
        break;

    case EV_WAKE_CONFIRMED: {
        ESP_LOGI(TAG, "=== 起床确认 → 触发晨间播报 ===");
        uint16_t now_min;
        uint8_t day;
        get_time_info(now_min, day);
        bc_on_wake_confirmed(&g_bc);
        bc_tick(&g_bc, now_min, day);
        break;
    }

    case EV_SEATED: {
        ESP_LOGI(TAG, "=== 入座检测 → 触发晚间播报 ===");
        // 测试用：直接下载播放 evening.opus
        caterpillar_http_play("evening", "2026-09-07");
        uint16_t now_min;
        uint8_t day;
        get_time_info(now_min, day);
        bc_on_seated(&g_bc, now_min);
        bc_tick(&g_bc, now_min, day);
        break;
    }

    case EV_AWAY:
        ESP_LOGI(TAG, "=== 离座 ===");
        break;

    case EV_STATE_CHANGED:
        ESP_LOGI(TAG, "FSM state -> %d", data);
        break;

    default:
        break;
    }
}

/* ---- 雷达读取任务 ---- */
static void radar_task(void* /*arg*/) {
    // 初始化 UART2
    uart_config_t uc = {
        .baud_rate = RADAR_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity   = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_driver_install(RADAR_UART_NUM, 2048, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(RADAR_UART_NUM, &uc));
    ESP_ERROR_CHECK(uart_set_pin(RADAR_UART_NUM,
        RADAR_TX_PIN, RADAR_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "Radar UART started: UART%d, RX=G%d, TX=G%d, %d baud",
             RADAR_UART_NUM, RADAR_RX_PIN, RADAR_TX_PIN, RADAR_BAUD);

    // 等雷达初始化完成，连发 3 次实时模式命令确保收到
    {
        vTaskDelay(pdMS_TO_TICKS(3000));
        uint8_t rt_cmd[] = {0x53,0x59,0x84,0x0F,0x00,0x01,0x00,0x40,0x54,0x43};
        for (int i = 0; i < 3; i++) {
            uart_write_bytes(RADAR_UART_NUM, rt_cmd, sizeof(rt_cmd));
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        ESP_LOGI(TAG, "Radar mode → real-time (sent x3)");
    }

    uint8_t buf[128];
    uint32_t last_fsm_tick = 0;
    uint32_t last_stats_ms = 0;
    uint32_t last_mode_check_ms = 0;
    bool daytime_mode = false;  // 当前是白天模式

    for (;;) {
        int n = uart_read_bytes(RADAR_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(50));
        uint32_t now = esp_log_timestamp();
        for (int i = 0; i < n; i++) {
            r60_feed(&g_parser, buf[i], now);
        }

        // 每 30 秒打印帧校验统计
        if (now - last_stats_ms >= 30000) {
            ESP_LOGI(TAG, "RADAR 校验统计 | 帧OK:%lu 帧错:%lu",
                     (unsigned long)g_parser.frames_ok,
                     (unsigned long)g_parser.frames_bad);
            last_stats_ms = now;
        }

        // 每 10 秒检查时间，自动切换白天/夜间模式 + 雷达上报模式
        if (now - last_mode_check_ms >= 10000) {
            time_t t = time(nullptr);
            struct tm tm_info;
            localtime_r(&t, &tm_info);
            int hour = tm_info.tm_hour;
            bool should_be_daytime = (hour >= 7 && hour < 21);

            if (should_be_daytime && !daytime_mode) {
                // 切白天：FSM 白天模式 + 雷达实时上报
                fsm_enter_day(&g_fsm, now);
                uint8_t realtime[] = {0x53,0x59,0x84,0x0F,0x00,0x01,0x00,
                                      (uint8_t)(0x53+0x59+0x84+0x0F+0x00+0x01+0x00),
                                      0x54,0x43};
                uart_write_bytes(RADAR_UART_NUM, realtime, sizeof(realtime));
                daytime_mode = true;
                ESP_LOGI(TAG, "=== 切换白天模式 (7:00-21:00) ===");
            } else if (!should_be_daytime && daytime_mode) {
                // 切夜间：FSM 睡眠监测 + 雷达睡眠上报
                fsm_enter_sleep_monitor(&g_fsm, now);
                uint8_t sleep[] = {0x53,0x59,0x84,0x0F,0x00,0x01,0x01,
                                   (uint8_t)(0x53+0x59+0x84+0x0F+0x00+0x01+0x01),
                                   0x54,0x43};
                uart_write_bytes(RADAR_UART_NUM, sleep, sizeof(sleep));
                daytime_mode = false;
                ESP_LOGI(TAG, "=== 切换夜间模式 (21:00-7:00) ===");
            }

            // 夜间 6:50 触发模拟闹钟（原型测试用）
            if (hour == 6 && tm_info.tm_min >= 50 &&
                fsm_state(&g_fsm) == FSM_SLEEP_MONITOR) {
                fsm_start_alarm(&g_fsm, now);
                ESP_LOGI(TAG, "=== 模拟闹钟：6:50 唤醒窗口 ===");
            }

            last_mode_check_ms = now;
        }

        // 每 200ms 驱动一次状态机
        if (now - last_fsm_tick >= 200) {
            fsm_tick(&g_fsm, r60_status(&g_parser), now);

            uint16_t now_min;
            uint8_t day;
            get_time_info(now_min, day);
            bc_tick(&g_bc, now_min, day);

            last_fsm_tick = now;
        }
    }
}

/* ---- 公开接口 ---- */

void caterpillar_radar_init(void) {
    // 1. 初始化雷达解析器
    r60_init(&g_parser, on_radar_frame, nullptr);
    ESP_LOGI(TAG, "Radar parser initialized");

    // 2. 初始化状态机（默认配置，夜间模式）
    fsm_config_t cfg;
    fsm_default_config(&cfg);
    fsm_init(&g_fsm, &cfg, on_fsm_event, nullptr);
    fsm_enter_day(&g_fsm, esp_log_timestamp());  // 默认白天模式（夜间自动切换）
    ESP_LOGI(TAG, "Radar FSM initialized (day mode)");

    // 3. 初始化播报调度（先设默认值，后续从 manifest 获取）
    bc_config_t bcfg = {
        .morning_deadline_min = 7 * 60 + 25,   // 07:25 前必须播
        .evening_start_min    = 20 * 60 + 30,   // 20:30 开始
        .evening_end_min      = 21 * 60 + 15,   // 21:15 结束
    };
    bc_init(&g_bc, &bcfg, on_broadcast_play, nullptr);
    ESP_LOGI(TAG, "Broadcast scheduler initialized");

    // 4. 启动雷达读取任务
    xTaskCreate(radar_task, "radar_task", 4096, nullptr, 5, nullptr);
    ESP_LOGI(TAG, "Radar task started");
}

void caterpillar_radar_register_mcp_tools(McpServer& mcp_server) {
    mcp_server.AddTool(
        "self.radar.get_status",
        "读取 R60ABD1 毫米波雷达实时状态：在位/体动幅度/呼吸率/"
        "心率/在床/睡眠分期/解析统计",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            const r60_status_t* s = r60_status(&g_parser);
            char json[256];
            snprintf(json, sizeof(json),
                "{\"presence\":%d,\"motion\":%d,\"body_move\":%d,"
                "\"distance_cm\":%d,\"breath_rate\":%d,\"heart_rate\":%d,"
                "\"in_bed\":%d,\"sleep_stage\":%d,\"fsm_state\":%d,"
                "\"frames_ok\":%lu,\"frames_bad\":%lu}",
                s->presence, s->motion, s->body_move,
                s->distance_cm, s->breath_rate, s->heart_rate,
                s->in_bed, s->sleep_stage, (int)fsm_state(&g_fsm),
                (unsigned long)g_parser.frames_ok,
                (unsigned long)g_parser.frames_bad);
            return std::string(json);
        });
}
