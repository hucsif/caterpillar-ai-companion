/*
 * caterpillar_mqtt.c — 毛毛虫 EMQX MQTT 实现
 *
 * 连接自建 EMQX broker，上报雷达语义事件和设备心跳。
 */
#include "caterpillar_mqtt.h"

#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <mqtt_client.h>

#define TAG "caterpillar_mqtt"

static esp_mqtt_client_handle_t g_client = NULL;
static char                     g_event_topic[80];
static char                     g_status_topic[80];
static caterpillar_mqtt_cmd_cb  g_cmd_cb = NULL;
static void                    *g_cmd_user = NULL;
static bool                     g_connected = false;

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "EMQX connected");
        g_connected = true;
        // 订阅下行指令
        esp_mqtt_client_subscribe_single(g_client, "dev/caterpillar/cmd", 1);
        ESP_LOGI(TAG, "Subscribed: dev/caterpillar/cmd");
        break;
    case MQTT_EVENT_DISCONNECTED:
        g_connected = false;
        break;
    case MQTT_EVENT_DATA:
        if (ev->topic_len > 0 && g_cmd_cb) {
            char payload[256];
            int len = ev->data_len < (int)sizeof(payload) - 1
                      ? ev->data_len : (int)sizeof(payload) - 1;
            memcpy(payload, ev->data, len);
            payload[len] = '\0';
            g_cmd_cb(payload, g_cmd_user);
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "EMQX error");
        break;
    default:
        break;
    }
}

bool caterpillar_mqtt_init(const char *broker_url) {
    if (g_client || !broker_url || !broker_url[0]) return false;

    // 设备 ID：用 MAC 地址
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char device_id[24];
    snprintf(device_id, sizeof(device_id),
             "cat-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    snprintf(g_event_topic, sizeof(g_event_topic),
             "dev/caterpillar/event");
    snprintf(g_status_topic, sizeof(g_status_topic),
             "dev/caterpillar/status");

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = broker_url,
        .credentials.client_id = device_id,
        .session.keepalive = 60,
        .network.disable_auto_reconnect = false,
    };

    g_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(g_client, ESP_EVENT_ANY_ID,
                                    mqtt_event_handler, NULL);
    esp_err_t err = esp_mqtt_client_start(g_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "MQTT started: %s", broker_url);
    return true;
}

void caterpillar_mqtt_deinit(void) {
    if (g_client) {
        esp_mqtt_client_destroy(g_client);
        g_client = NULL;
    }
    g_connected = false;
}

bool caterpillar_mqtt_is_connected(void) { return g_connected; }

bool caterpillar_mqtt_publish_event(const char *payload_json) {
    if (!g_client || !g_connected) return false;
    int msg_id = esp_mqtt_client_publish(g_client, g_event_topic,
                                          payload_json, strlen(payload_json), 1, 0);
    return msg_id >= 0;
}

bool caterpillar_mqtt_publish_status(uint32_t free_heap,
                                      uint32_t uptime_s,
                                      int8_t   wifi_rssi) {
    if (!g_client || !g_connected) return false;
    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\"free_heap\":%lu,\"uptime_s\":%lu,\"rssi\":%d}",
             (unsigned long)free_heap, (unsigned long)uptime_s, wifi_rssi);
    int msg_id = esp_mqtt_client_publish(g_client, g_status_topic,
                                          payload, strlen(payload), 1, 0);
    return msg_id >= 0;
}

void caterpillar_mqtt_on_command(caterpillar_mqtt_cmd_cb cb, void *user) {
    g_cmd_cb = cb;
    g_cmd_user = user;
}
