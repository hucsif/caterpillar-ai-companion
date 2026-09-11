/*
 * caterpillar_config.cc — 毛毛虫端点统一配置实现
 *
 * 从 NVS websocket.url（如 ws://192.168.3.43:8000/xiaozhi/v1/）
 * 提取 host，推导出 mqtt://host:1883 和 http://host:8001。
 */
#include "caterpillar_config.h"

#include <cstring>
#include <esp_log.h>
#include "settings.h"

static const char* TAG = "cat_config";
static caterpillar_endpoints_t g_endpoints = {};
static bool g_initialized = false;

bool caterpillar_config_init(caterpillar_endpoints_t* out) {
    if (g_initialized) {
        if (out) *out = g_endpoints;
        return true;
    }

    Settings ws("websocket", false);
    std::string ws_url = ws.GetString("url");
    if (ws_url.empty()) {
        ESP_LOGW(TAG, "websocket.url not configured in NVS");
        return false;
    }

    size_t proto_end = ws_url.find("://");
    if (proto_end == std::string::npos) {
        ESP_LOGW(TAG, "Invalid websocket URL: %s", ws_url.c_str());
        return false;
    }

    size_t host_start = proto_end + 3;
    size_t port_start = ws_url.find(':', host_start);
    size_t path_start = ws_url.find('/', host_start);

    /* 提取 host：截断到端口冒号或路径斜杠 whichever 先出现 */
    size_t host_end;
    if (port_start != std::string::npos &&
        (path_start == std::string::npos || port_start < path_start)) {
        host_end = port_start;
    } else if (path_start != std::string::npos) {
        host_end = path_start;
    } else {
        host_end = ws_url.length();
    }

    std::string host = ws_url.substr(host_start, host_end - host_start);
    if (host.empty()) {
        ESP_LOGW(TAG, "Empty host in websocket URL: %s", ws_url.c_str());
        return false;
    }

    strncpy(g_endpoints.host, host.c_str(), sizeof(g_endpoints.host) - 1);
    g_endpoints.host[sizeof(g_endpoints.host) - 1] = '\0';
    snprintf(g_endpoints.mqtt_url, sizeof(g_endpoints.mqtt_url),
             "mqtt://%s:1883", g_endpoints.host);
    snprintf(g_endpoints.server_url, sizeof(g_endpoints.server_url),
             "http://%s:8001", g_endpoints.host);

    g_initialized = true;
    ESP_LOGI(TAG, "Endpoints resolved: mqtt=%s server=%s",
             g_endpoints.mqtt_url, g_endpoints.server_url);

    if (out) *out = g_endpoints;
    return true;
}

const caterpillar_endpoints_t* caterpillar_config_get(void) {
    return g_initialized ? &g_endpoints : nullptr;
}
