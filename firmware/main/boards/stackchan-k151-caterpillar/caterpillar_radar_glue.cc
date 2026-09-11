/*
 * caterpillar_radar_glue.cc — 毛毛虫智感 核心胶水层
 *
 * 改造要点（2026-07-24）：
 *   1. 拆分上帝任务：radar_task(核1,P7,纯实时) + broadcast_task(P4,播报) + net_task(P3,MQTT)
 *   2. 播报下载异步化：radar_task 只投递事件到队列，HTTP 下载在 broadcast_task 阻塞
 *   3. bc_tick 仅在 broadcast_task 调用，消除重复调用
 *   4. MQTT 初始化移到 net_task，不再耦合在 radar_task 循环里
 *   5. URL 解析改用 caterpillar_config 统一模块
 *   6. MQTT 事件 JSON 改用 cJSON 构造
 *
 * 数据流：
 *   UART ISR ─→ r60abd1.c 帧解析
 *                  ↓
 *              radar_fsm.c 状态机（200ms tick）
 *                  ↓ on_fsm_event
 *         ┌────────┴────────┐
 *         ↓                 ↓
 *   g_bc_queue          MQTT publish（非阻塞）
 *   (播报事件)          (事件上报)
 *         ↓
 *   broadcast_task
 *   · bc_on_wake_confirmed / bc_on_seated
 *   · bc_tick（deadline兜底 + 重试）
 *   · on_broadcast_play → caterpillar_http_play（缓存优先）
 */
#include "caterpillar_radar_glue.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_timer.h>
#include <cJSON.h>

#include "config.h"
#include "application.h"
#include "mcp_server.h"
#include "r60abd1.h"
#include "radar_fsm.h"
#include "broadcast_sched.h"
#include "cache_mgr.h"
#include "caterpillar_mqtt.h"
#include "caterpillar_http.h"
#include "caterpillar_config.h"
#include "ld2450.h"
#include "wave_fsm.h"

static const char* TAG = "caterpillar_radar";

/* ---- 全局实例 ---- */
static r60_parser_t   g_parser;
static ld2450_parser_t g_ld2450;
static radar_fsm_t    g_fsm;
static bc_sched_t     g_bc;
static caterpillar_night_mode_fn g_night_cb = nullptr;
static caterpillar_head_angle_fn g_head_cb = nullptr;
static bool g_night_mode = false;
static bool g_tracking_enabled = false;  // 默认关闭，语音开启追踪

/* ---- 手势 FSM 实例 ---- */
static squat_fsm_t g_squat;
static wave_fsm_t  g_wave;

static void on_wave_event(wave_event_t ev, int data, void* /*user*/) {
    switch (ev) {
    case WAVE_EV_DETECTED:
        ESP_LOGI(TAG, "=== WAVE detected (events=%d) ===", data);
        break;
    case WAVE_EV_ENDED:
        ESP_LOGI(TAG, "=== WAVE ended ===");
        break;
    }
}

void caterpillar_set_head_angle_callback(caterpillar_head_angle_fn cb) {
    g_head_cb = cb;
}

int16_t caterpillar_get_nearest_person_yaw(void) {
    /* LD2450 10Hz x/y → 水平方位角，忽略 3 米以外的目标 */
    const ld2450_status_t* l = ld2450_status(&g_ld2450);
    for (int i = 0; i < LD2450_TARGET_COUNT; i++) {
        if (l->targets[i].valid && l->targets[i].y_mm > 0 && l->targets[i].y_mm < 3000) {
            float deg = atan2f((float)l->targets[i].x_mm,
                               (float)l->targets[i].y_mm) * 57.29578f;
            if (deg < -90) deg = -90;
            if (deg >  90) deg =  90;
            return (int16_t)deg;
        }
    }
    return INT16_MIN;
}

int16_t caterpillar_z_to_pitch(void) {
    /* R60ABD1 z轴不可用，固定 pitch=45 */
    return 45;
}

/* ---- 播报事件队列：radar_task → broadcast_task ---- */
typedef enum {
    BC_MSG_WAKE_CONFIRMED = 0,
    BC_MSG_SEATED = 1,
} bc_msg_type_t;

typedef struct {
    bc_msg_type_t type;
} bc_msg_t;

static QueueHandle_t g_bc_queue = nullptr;

/* ---- 时间工具 ---- */
static void get_time_info(uint16_t& out_minutes, uint8_t& out_day) {
    time_t now = time(nullptr);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    out_minutes = (uint16_t)(tm_info.tm_hour * 60 + tm_info.tm_min);
    out_day = (uint8_t)tm_info.tm_mday;
}

static void get_date_str(char* out, size_t out_len) {
    time_t now = time(nullptr);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    strftime(out, out_len, "%Y-%m-%d", &tm_info);
}

/* ---- MQTT 事件上报（用 cJSON 构造） ---- */
static void publish_event(const char* ev_name) {
    cJSON* root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "ev", ev_name);
    char* str = cJSON_PrintUnformatted(root);
    if (str) {
        caterpillar_mqtt_publish_event(str);
        free(str);
    }
    cJSON_Delete(root);
}

/* ---- 播报回调：缓存优先，HTTP 兜底（实际逻辑在 caterpillar_http_play 内） ---- */
static bool on_broadcast_play(bc_slot_t slot, void* /*user*/) {
    const char* type = (slot == SLOT_MORNING) ? "morning" : "evening";
    char today[11];
    get_date_str(today, sizeof(today));
    ESP_LOGI(TAG, "Broadcast trigger: %s (date=%s)", type, today);
    /* caterpillar_http_play 内部已实现缓存优先逻辑 */
    return caterpillar_http_play(type, today);
}

/* ---- 雷达帧回调 ---- */
static void on_radar_frame(uint8_t ctl, uint8_t cmd,
                           const uint8_t* data, uint16_t len, void* /*user*/) {
    const r60_status_t* s = r60_status(&g_parser);
    uint32_t now = esp_log_timestamp();

    /* 蹲下检测：按 cmd 分发到 FSM */
    if (ctl == 0x80) {
        if (cmd == 0x03)          squat_on_body_move(&g_squat, s->body_move, now);
        else if (cmd == 0x05)     squat_on_position(&g_squat, s->x_cm, s->y_cm, now);
    }

    switch (ctl) {
    case 0x80:
        /* presence/body_move/position 的日志 */
        if (cmd == 0x01) {
            ESP_LOGI(TAG, "RADAR > presence:%d body_move:%d xy=(%d,%d)",
                     s->presence, s->body_move, s->x_cm, s->y_cm);
        } else if (cmd == 0x03) {
            ESP_LOGI(TAG, "RADAR > body_move:%d", s->body_move);
        } else if (cmd == 0x05) {
            ESP_LOGI(TAG, "RADAR > x=%dcm y=%dcm", s->x_cm, s->y_cm);
        }
        break;
    case 0x81:
        ESP_LOGD(TAG, "RADAR > breath:%d/min", s->breath_rate);
        break;
    case 0x84:
        if (cmd == 0x01)
            ESP_LOGI(TAG, "RADAR > in_bed:%d", s->in_bed);
        else if (cmd == 0x02)
            ESP_LOGI(TAG, "RADAR > sleep_stage:%d(0deep/1light/2awake)", s->sleep_stage);
        else if (cmd == 0x0F)
            ESP_LOGI(TAG, "RADAR > mode reply: 0x%02x", data[0]);
        break;
    case 0x85:
        ESP_LOGD(TAG, "RADAR > heart:%d/min", s->heart_rate);
        break;
    default:
        break;
    }
}

/* ---- 状态机事件回调：MQTT 上报 + 投递播报事件到队列（零阻塞） ---- */
static void on_fsm_event(fsm_event_t ev, int data, void* /*user*/) {
    switch (ev) {
    case EV_ALARM_STOP:
        ESP_LOGI(TAG, "=== alarm_stop: sit-up confirmed ===");
        publish_event("alarm_stop");
        break;

    case EV_ALARM_RESUME:
        ESP_LOGI(TAG, "=== alarm_resume: back to sleep ===");
        publish_event("alarm_resume");
        break;

    case EV_WAKE_CONFIRMED:
        ESP_LOGI(TAG, "=== wake_confirmed -> morning broadcast ===");
        publish_event("wake_confirmed");
        /* 投递到 broadcast_task，不在此处直接播放 */
        if (g_bc_queue) {
            bc_msg_t msg = {.type = BC_MSG_WAKE_CONFIRMED};
            xQueueSend(g_bc_queue, &msg, 0);  /* 零等待，满了就丢 */
        }
        break;

    case EV_SEATED:
        ESP_LOGI(TAG, "=== seated -> evening broadcast ===");
        publish_event("seated");
        if (g_bc_queue) {
            bc_msg_t msg = {.type = BC_MSG_SEATED};
            xQueueSend(g_bc_queue, &msg, 0);
        }
        break;

    case EV_AWAY:
        ESP_LOGI(TAG, "=== away ===");
        publish_event("away");
        break;

    case EV_STATE_CHANGED:
        ESP_LOGI(TAG, "FSM state -> %d", data);
        break;

    case EV_SQUAT:
        ESP_LOGI(TAG, "=== SQUAT detected (body_move=%d) ===", data);
        break;
    case EV_SQUAT_END:
        ESP_LOGI(TAG, "=== SQUAT ended ===");
        break;

    default:
        break;
    }
}

/* ---- 夜间模式 ---- */
static void set_night_mode(bool night) {
    g_night_mode = night;
    if (g_night_cb) g_night_cb(night);
    ESP_LOGI(TAG, "Night mode: %s", night ? "ON" : "OFF");
}

/* ---- R60 命令发送（无ACK，主循环查询用） ---- */
static void r60_send_cmd_noack(uint8_t ctl, uint8_t cmd) {
    uint8_t cmd_buf[9];
    int len = r60_build_cmd(cmd_buf, ctl, cmd, NULL, 0);
    uart_write_bytes(RADAR_UART_NUM, cmd_buf, len);
}

/* ---- 发送雷达模式切换命令 ---- */
static void send_radar_mode(uint8_t mode_val) {
    uint8_t cmd[] = {0x53, 0x59, 0x84, 0x0F, 0x00, 0x01, mode_val,
                     (uint8_t)(0x53 + 0x59 + 0x84 + 0x0F + 0x00 + 0x01 + mode_val),
                     0x54, 0x43};
    uart_write_bytes(RADAR_UART_NUM, cmd, sizeof(cmd));
}

/*====================================================================
 * 任务 1：radar_task — 纯实时，零网络 IO
 * 核1，优先级 7，栈 4KB
 * 职责：UART 读取 + 帧解析 + FSM tick + 模式切换 + 闹钟触发
 *====================================================================*/
static void radar_task(void* /*arg*/) {
    /* 初始化 UART2 */
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

    /* 初始化 LD2450 UART（UART0 重映射到 PORT.B G8/G9, 10Hz yaw追踪） */
    {
        uart_config_t uc2 = {
            .baud_rate = LD2450_BAUD,
            .data_bits = UART_DATA_8_BITS,
            .parity   = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        };
        ESP_ERROR_CHECK(uart_driver_install(LD2450_UART_NUM, 1024, 0, 0, nullptr, 0));
        ESP_ERROR_CHECK(uart_param_config(LD2450_UART_NUM, &uc2));
        ESP_ERROR_CHECK(uart_set_pin(LD2450_UART_NUM,
            LD2450_TX_PIN, LD2450_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
        ESP_LOGI(TAG, "LD2450 UART started: UART%d, RX=G%d, TX=G%d, %d baud",
                 LD2450_UART_NUM, LD2450_RX_PIN, LD2450_TX_PIN, LD2450_BAUD);
    }

    /* 等雷达初始化完成，连发 3 次实时模式命令 */
    vTaskDelay(pdMS_TO_TICKS(3000));
    for (int i = 0; i < 3; i++) {
        send_radar_mode(0x00);  /* 0x00 = real-time mode */
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "Radar mode -> real-time (sent x3)");

    uint8_t buf[128];
    uint32_t last_fsm_tick = 0;
    uint32_t last_mode_check_ms = 0;
    bool daytime_mode = false;

    for (;;) {
        /* 50ms 超时读取 UART，非阻塞 */
        int n = uart_read_bytes(RADAR_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(50));
        uint32_t now = esp_log_timestamp();

        for (int i = 0; i < n; i++) {
            r60_feed(&g_parser, buf[i], now);
        }

        /* LD2450 帧解析（10fps, yaw追踪），buf=128 匹配 UART FIFO 深度防溢出 */
        uint8_t buf2[128];
        int n2 = uart_read_bytes(LD2450_UART_NUM, buf2, sizeof(buf2), pdMS_TO_TICKS(10));
        for (int i = 0; i < n2; i++) {
            ld2450_feed(&g_ld2450, buf2[i], now);
        }

        /* 手势检测：蹲下在 radar 回调(R60) + 挥手在 LD2450 回调，主循环无需再处理 */

        /* 转头追踪：LD2450 x/y→yaw(10Hz) + R60ABD1 z→pitch(2s更新)，带死区滤波 */
        if (g_head_cb && g_tracking_enabled) {
            const r60_status_t* s = r60_status(&g_parser);
            if (s->presence) {
                int16_t yaw = caterpillar_get_nearest_person_yaw();
                int16_t pitch = caterpillar_z_to_pitch();
                if (yaw != INT16_MIN) {
                    static int16_t last_yaw = 0, last_pitch = 0;
                    static uint32_t last_send_ms = 0;
                    uint32_t now_ms = esp_log_timestamp();
                    if (std::abs(yaw - last_yaw) >= 2 || std::abs(pitch - last_pitch) >= 2
                        || now_ms - last_send_ms >= 400) {
                        g_head_cb(yaw, pitch);
                        last_yaw = yaw;
                        last_pitch = pitch;
                        last_send_ms = now_ms;
                    }
                }
            }
        }

        /* 每 10 秒：模式切换 + 闹钟触发 */
        if (now - last_mode_check_ms >= 10000) {
            time_t t = time(nullptr);
            struct tm tm_info;
            localtime_r(&t, &tm_info);
            int hour = tm_info.tm_hour;
            bool should_be_daytime = (hour >= 7 && hour < 21);

            if (should_be_daytime && !daytime_mode) {
                set_night_mode(false);
                fsm_enter_day(&g_fsm, now);
                send_radar_mode(0x00);  /* real-time */
                daytime_mode = true;
                ESP_LOGI(TAG, "=== Day mode (7:00-21:00) ===");
            } else if (!should_be_daytime && daytime_mode) {
                set_night_mode(true);
                fsm_enter_sleep_monitor(&g_fsm, now);
                send_radar_mode(0x01);  /* sleep monitoring */
                daytime_mode = false;
                ESP_LOGI(TAG, "=== Night mode (21:00-7:00) ===");
            }

            /* 夜间 6:50 触发模拟闹钟（原型测试用） */
            if (hour == 6 && tm_info.tm_min >= 50 &&
                fsm_state(&g_fsm) == FSM_SLEEP_MONITOR) {
                fsm_start_alarm(&g_fsm, now);
                ESP_LOGI(TAG, "=== Simulated alarm: 6:50 wake window ===");
            }

            last_mode_check_ms = now;
        }

        /* 每 200ms 驱动一次状态机（bc_tick 已移至 broadcast_task） */
        if (now - last_fsm_tick >= 200) {
            fsm_tick(&g_fsm, r60_status(&g_parser), now);
            last_fsm_tick = now;
        }
    }
}

/*====================================================================
 * 任务 2：broadcast_task — 播报调度 + 缓存播放 + HTTP 下载
 * 任意核，优先级 4，栈 6KB
 * 职责：消费播报事件队列 + bc_tick + on_broadcast_play
 * 阻塞只影响自身，不影响雷达
 *====================================================================*/
static void broadcast_task(void* /*arg*/) {
    uint32_t last_bc_tick = 0;

    for (;;) {
        /* 等待播报事件，最多等 200ms 然后做 bc_tick */
        bc_msg_t msg;
        BaseType_t got = xQueueReceive(g_bc_queue, &msg, pdMS_TO_TICKS(200));

        if (got == pdPASS) {
            /* 处理播报事件 */
            uint16_t now_min;
            uint8_t day;
            get_time_info(now_min, day);

            if (msg.type == BC_MSG_WAKE_CONFIRMED) {
                bc_on_wake_confirmed(&g_bc);
            } else if (msg.type == BC_MSG_SEATED) {
                bc_on_seated(&g_bc, now_min);
            }
            /* bc_on_wake_confirmed/bc_on_seated 内部 try_play → on_broadcast_play
             * 可能触发 HTTP 下载（在此任务阻塞，不影响 radar_task） */
        }

        /* 每 200ms 做一次 bc_tick（deadline 兜底 + pending 重试 + 跨天复位） */
        uint32_t now = esp_log_timestamp();
        if (now - last_bc_tick >= 200) {
            uint16_t now_min;
            uint8_t day;
            get_time_info(now_min, day);
            bc_tick(&g_bc, now_min, day);
            last_bc_tick = now;
        }
    }
}

/*====================================================================
 * 任务 3：net_task — MQTT 初始化 + 心跳上报
 * 任意核，优先级 3，栈 4KB
 * 职责：等 WiFi 就绪 → 初始化 MQTT → 30s 周期心跳
 *====================================================================*/
static void net_task(void* /*arg*/) {
    /* 等 WiFi 就绪（原型用固定延时，量产可改为 WiFi 事件回调） */
    vTaskDelay(pdMS_TO_TICKS(30000));

    /* 初始化端点配置 + MQTT */
    caterpillar_endpoints_t ep;
    if (caterpillar_config_init(&ep)) {
        caterpillar_mqtt_init(ep.mqtt_url);
    } else {
        ESP_LOGW(TAG, "Config init failed, MQTT disabled");
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));  /* 30s 心跳 */

        if (caterpillar_mqtt_is_connected()) {
            ESP_LOGI(TAG, "RADAR stats | frames_ok:%lu frames_bad:%lu",
                     (unsigned long)g_parser.frames_ok,
                     (unsigned long)g_parser.frames_bad);

            caterpillar_mqtt_publish_status(
                esp_get_free_heap_size(),
                (uint32_t)(esp_log_timestamp() / 1000),
                0);
        }
    }
}

/*====================================================================
 * 任务 4：cache_sync_task — 每日缓存预下载（保持独立）
 *====================================================================*/
static void cache_sync_task(void* /*arg*/) {
    vTaskDelay(pdMS_TO_TICKS(30000));  /* 开机 30 秒后 */

    for (;;) {
        time_t now = time(nullptr);
        struct tm tm_info;
        localtime_r(&now, &tm_info);
        int min_of_day = tm_info.tm_hour * 60 + tm_info.tm_min;

        if (min_of_day >= 4 * 60 && min_of_day < 6 * 60) {
            ESP_LOGI(TAG, "Daily cache sync starting...");
            int ret = cache_mgr_sync();
            ESP_LOGI(TAG, "Cache sync result: %d (0=ok, >0=no_update, <0=error)", ret);

            uint32_t total, used;
            cache_mgr_stats(&total, &used);
            ESP_LOGI(TAG, "Cache partition: %lu/%lu bytes used (%d files)",
                     (unsigned long)used, (unsigned long)total, cache_mgr_count());
        }

        vTaskDelay(pdMS_TO_TICKS(30 * 60 * 1000));  /* 每 30 分钟检查一次 */
    }
}

/* ---- 公开接口 ---- */

void caterpillar_set_night_mode_callback(caterpillar_night_mode_fn cb) {
    g_night_cb = cb;
}

void caterpillar_set_server_url(const char* url) {
    /* 外部显式指定 URL 时覆盖配置模块的推导结果 */
    cache_mgr_init(url);
    caterpillar_http_init(url);
}

/* MQTT 下行指令处理 */
static void on_mqtt_cmd(const char *payload_json, void * /*user*/) {
    cJSON *root = cJSON_Parse(payload_json);
    if (!root) return;
    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "play_announcement") == 0) {
        cJSON *url_j = cJSON_GetObjectItem(root, "url");
        cJSON *text_j = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(url_j)) {
            ESP_LOGI(TAG, "Announcement: %s", text_j ? text_j->valuestring : "");
            caterpillar_http_play_url(url_j->valuestring);
        }
    }
    cJSON_Delete(root);
}

void caterpillar_radar_init(void) {
    /* 注册 MQTT 下行指令回调 */
    caterpillar_mqtt_on_command(on_mqtt_cmd, nullptr);

    /* 1. 初始化端点配置（从 NVS websocket.url 解析） */
    caterpillar_endpoints_t ep;
    bool cfg_ok = caterpillar_config_init(&ep);

    /* 2. 初始化缓存管理器 + HTTP 模块（用统一配置） */
    if (cfg_ok) {
        cache_mgr_init(ep.server_url);
        caterpillar_http_init(ep.server_url);
    } else {
        ESP_LOGW(TAG, "Config not ready, cache/http will init later");
    }

    /* 3. 初始化雷达解析器 */
    r60_init(&g_parser, on_radar_frame, nullptr);
    ld2450_init(&g_ld2450, [](const ld2450_target_t* targets, int count, void*) {
        uint32_t now = esp_log_timestamp();
        static uint32_t last_log_ms = 0;

        /* 挥手检测：找第一个有效目标喂 wave_fsm */
        bool fed = false;
        for (int i = 0; i < count; i++) {
            if (targets[i].valid && targets[i].y_mm > 0 && targets[i].y_mm < 3000) {
                wave_on_frame(&g_wave, true,
                              targets[i].x_mm, targets[i].y_mm,
                              targets[i].speed_cms, now);
                fed = true;
                break;
            }
        }
        if (!fed) wave_on_frame(&g_wave, false, 0, 0, 0, now);

        /* 500ms 限流打印 */
        if (now - last_log_ms < 500) return;
        last_log_ms = now;
        for (int i = 0; i < count; i++) {
            if (targets[i].valid) {
                ESP_LOGI(TAG, "LD2450 ▶ T%d: x=%dmm y=%dmm speed=%dcm/s",
                         i + 1, targets[i].x_mm, targets[i].y_mm, targets[i].speed_cms);
            }
        }
    }, nullptr);
    ESP_LOGI(TAG, "Radar parsers initialized (R60ABD1 + LD2450)");

    /* 4. 初始化状态机 */
    fsm_config_t cfg;
    fsm_default_config(&cfg);
    fsm_init(&g_fsm, &cfg, on_fsm_event, nullptr);
    fsm_enter_day(&g_fsm, esp_log_timestamp());
    ESP_LOGI(TAG, "Radar FSM initialized (day mode)");

    /* 5. 播报调度 — 已关闭（使用小智云端TTS替代） */
    /* 6. 手势识别 */
    squat_init(&g_squat, NULL, on_fsm_event, nullptr);
    wave_init(&g_wave, NULL, on_wave_event, nullptr);
    ESP_LOGI(TAG, "Gesture FSMs initialized (squat + wave)");

    /* 7. 仅启动 radar_task（radar UART 解析 + LD2450 追踪） */
    xTaskCreatePinnedToCore(radar_task, "radar_task", 4096, nullptr, 7, nullptr, 1);
    ESP_LOGI(TAG, "Radar task started (P7, core1)");
}

void caterpillar_radar_register_mcp_tools(McpServer& mcp_server) {
    /* 手势状态查询（持续监测，即时返回） */
    mcp_server.AddTool(
        "self.gesture.get_status",
        "Check what the person is currently doing (continuously monitored). "
        "Returns: wave, squat, or none. "
        "Use when asked: what am I doing, am I waving, am I squatting, look at me.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            cJSON* root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "wave", wave_is_active(&g_wave) ? "wave" : "none");
            cJSON_AddStringToObject(root, "squat", squat_is_active(&g_squat) ? "squat" : "none");
            return root;
        });

    mcp_server.AddTool(
        "self.radar.track_me",
        "Turn on/off face tracking. When ON, the robot head automatically "
        "turns to face the nearest person detected by LD2450 radar. "
        "Use when asked: look at me, follow me, watch me, stop tracking, stop following.",
        PropertyList({Property("enable", kPropertyTypeBoolean)}),
        [](const PropertyList& props) -> ReturnValue {
            g_tracking_enabled = props["enable"].value<bool>();
            cJSON* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "ok", true);
            cJSON_AddBoolToObject(root, "tracking", g_tracking_enabled);
            return root;
        });

    mcp_server.AddTool(
        "self.radar.get_status",
        "Read both R60ABD1 sleep radar AND LD2450 tracking radar in real time. "
        "Returns: presence, distance_cm, body_move(0-100), breath_rate, heart_rate, "
        "in_bed, sleep_stage(0=deep 1=light 2=awake), night_mode, "
        "AND tracking data: track_x_mm, track_y_mm, track_speed_cms. "
        "Use for ANY question about position, location, distance, direction, "
        "whether someone is here, sleep, health, breathing, heart rate.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            const r60_status_t* s = r60_status(&g_parser);
            uint32_t total, used;
            cache_mgr_stats(&total, &used);

            cJSON* root = cJSON_CreateObject();
            cJSON_AddNumberToObject(root, "presence", s->presence);
            cJSON_AddNumberToObject(root, "motion", s->motion);
            cJSON_AddNumberToObject(root, "body_move", s->body_move);
            cJSON_AddNumberToObject(root, "x_cm", s->x_cm);
            cJSON_AddNumberToObject(root, "y_cm", s->y_cm);
            cJSON_AddNumberToObject(root, "breath_rate", s->breath_rate);
            cJSON_AddNumberToObject(root, "heart_rate", s->heart_rate);
            cJSON_AddNumberToObject(root, "in_bed", s->in_bed);
            cJSON_AddNumberToObject(root, "sleep_stage", s->sleep_stage);
            cJSON_AddNumberToObject(root, "fsm_state", (int)fsm_state(&g_fsm));
            cJSON_AddNumberToObject(root, "night_mode", g_night_mode ? 1 : 0);
            cJSON_AddNumberToObject(root, "frames_ok", (double)g_parser.frames_ok);
            cJSON_AddNumberToObject(root, "frames_bad", (double)g_parser.frames_bad);
            cJSON_AddNumberToObject(root, "cache_files", cache_mgr_count());
            cJSON_AddNumberToObject(root, "cache_used", (double)used);
            cJSON_AddNumberToObject(root, "cache_total", (double)total);

            /* LD2450 追踪数据已禁用——使用 R60ABD1 x_cm/y_cm/z_cm 替代 */
            return root;
        });

    /* LD2450 追踪工具已禁用——使用 R60ABD1 self.radar.get_status 替代 */
#if 0
    mcp_server.AddTool(
        "self.radar.ld2450_targets",
        "Read HLK-LD2450 tracking radar: returns up to 3 targets with X/Y coordinates(mm) "
        "and speed(cm/s). Use to answer: where is the person, which direction, how far, "
        "or how fast they are moving. X=horizontal(left-negative,right-positive), Y=distance(forward).",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            const ld2450_status_t* s = ld2450_status(&g_ld2450);
            cJSON* root = cJSON_CreateObject();
            cJSON* arr = cJSON_CreateArray();
            for (int i = 0; i < LD2450_TARGET_COUNT; i++) {
                if (s->targets[i].valid) {
                    cJSON* t = cJSON_CreateObject();
                    cJSON_AddNumberToObject(t, "id", i + 1);
                    cJSON_AddNumberToObject(t, "x_mm", s->targets[i].x_mm);
                    cJSON_AddNumberToObject(t, "y_mm", s->targets[i].y_mm);
                    cJSON_AddNumberToObject(t, "speed_cms", s->targets[i].speed_cms);
                    cJSON_AddItemToArray(arr, t);
                }
            }
            cJSON_AddItemToObject(root, "targets", arr);
            cJSON_AddNumberToObject(root, "frames_ok", (double)g_ld2450.frames_ok);
            cJSON_AddNumberToObject(root, "frames_bad", (double)g_ld2450.frames_bad);
            return root;
        });
#endif

    mcp_server.AddTool(
        "self.radar.night_mode",
        "Toggle night mode: true=screen off+servo release+LED off, false=normal",
        PropertyList({Property("enable", kPropertyTypeBoolean)}),
        [](const PropertyList& props) -> ReturnValue {
            bool enable = props["enable"].value<bool>();
            set_night_mode(enable);
            // 返回 JSON 结果给 LLM
            cJSON* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "ok", true);
            cJSON_AddBoolToObject(root, "night_mode", enable);
            return root;
        });
}
