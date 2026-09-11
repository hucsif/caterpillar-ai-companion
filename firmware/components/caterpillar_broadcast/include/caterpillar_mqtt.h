/*
 * caterpillar_mqtt.h — 毛毛虫 EMQX MQTT 事件上报
 *
 * 协议（对应《软件开发方案》4.2）：
 *   上行  dev/{id}/event   QoS1  雷达语义事件
 *         dev/{id}/status  QoS1  心跳
 *   下行  dev/{id}/cmd     QoS1  云端指令
 */
#ifndef CATERPILLAR_MQTT_H
#define CATERPILLAR_MQTT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool caterpillar_mqtt_init(const char *broker_url);
void caterpillar_mqtt_deinit(void);
bool caterpillar_mqtt_is_connected(void);
bool caterpillar_mqtt_publish_event(const char *payload_json);
bool caterpillar_mqtt_publish_status(uint32_t free_heap,
                                      uint32_t uptime_s,
                                      int8_t   wifi_rssi);
typedef void (*caterpillar_mqtt_cmd_cb)(const char *payload_json, void *user);
void caterpillar_mqtt_on_command(caterpillar_mqtt_cmd_cb cb, void *user);

#ifdef __cplusplus
}
#endif
#endif
