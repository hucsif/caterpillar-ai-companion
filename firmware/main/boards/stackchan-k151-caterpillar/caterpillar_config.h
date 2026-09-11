/*
 * caterpillar_config.h — 毛毛虫端点统一配置
 *
 * 从 NVS websocket.url 一次性解析出 MQTT broker 和内容服务器地址，
 * 消除 glue.cc 与 http.cc 中重复的 URL 解析逻辑。
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char mqtt_url[80];    /* "mqtt://host:1883" */
    char server_url[128]; /* "http://host:8001" */
    char host[64];        /* 纯 host，供其他用途 */
} caterpillar_endpoints_t;

/* 从 NVS websocket.url 解析端点配置。
 * 可重复调用，仅首次实际解析，后续返回缓存结果。
 * 返回 true 表示解析成功，out 可为 NULL（仅触发解析）。 */
bool caterpillar_config_init(caterpillar_endpoints_t* out);

/* 获取上次解析的结果（不重新解析）。未初始化时返回 NULL。 */
const caterpillar_endpoints_t* caterpillar_config_get(void);

#ifdef __cplusplus
}
#endif
