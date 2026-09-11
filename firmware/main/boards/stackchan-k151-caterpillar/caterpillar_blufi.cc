/*
 * caterpillar_blufi.cc — 毛毛虫智感 BLE 蓝牙配网
 *
 * 包装上游 boards/common/blufi.cpp（932行，已编译进固件），提供：
 *   1. 无WiFi时自动启动 BLE 广播（设备名 Xiaozhi-Blufi）
 *   2. MCP工具 self.wifi.blufi_start —— 家长手动触发配网
 *   3. MCP工具 self.wifi.blufi_status —— 查看当前配网状态
 *
 * 配网流程: 手机打开ESP BluFi App → 扫描BLE → 选WiFi输密码 → 设备连网
 * 安全: #ifdef CONFIG_BT_ENABLED 包裹，BT未启用时编译为空操作
 *
 * 依赖: boards/common/blufi.cpp（需menuconfig启用BT+Bluedroid+BLUFI）
 * 文件: stackchan-mcp/firmware/main/boards/stackchan-k151-caterpillar/
 * 创建: 2026-07-23
 */

#include "caterpillar_blufi.h"

#include <cstdio>
#include <esp_log.h>
#include <esp_wifi.h>

#ifdef CONFIG_BT_ENABLED
#include "blufi.h"
#endif
#include "application.h"
#include "mcp_server.h"

static const char* TAG = "caterpillar_blufi";
static bool g_blufi_started = false;

void caterpillar_blufi_init(void) {
#ifdef CONFIG_BT_ENABLED
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        ESP_LOGI(TAG, "WiFi already connected, BluFi not needed");
        return;
    }

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
        ESP_LOGI(TAG, "Already in WiFi config mode (captive portal), skip BluFi");
        return;
    }

    ESP_LOGI(TAG, "No WiFi connection, starting BluFi BLE provisioning...");
    esp_err_t ret = Blufi::GetInstance().init();
    if (ret == ESP_OK) {
        g_blufi_started = true;
        ESP_LOGI(TAG, "BluFi started — 等待家长配网");
    } else {
        ESP_LOGW(TAG, "BluFi init failed: %s", esp_err_to_name(ret));
    }
#else
    ESP_LOGI(TAG, "BT not enabled in sdkconfig, BluFi skipped. "
             "Use menuconfig → Component config → Bluetooth → enable BT + NimBLE");
#endif
}

void caterpillar_blufi_register_mcp_tools(McpServer& mcp_server) {
#ifdef CONFIG_BT_ENABLED
    mcp_server.AddTool(
        "self.wifi.blufi_start",
        "启动 BluFi BLE 配网（家长用手机 App 或小程序给设备配置 WiFi）",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            if (g_blufi_started) {
                cJSON* root = cJSON_CreateObject();
                cJSON_AddBoolToObject(root, "ok", true);
                cJSON_AddStringToObject(root, "msg", "BluFi already running");
                return root;
            }
            Blufi::GetInstance().deinit();
            esp_err_t ret = Blufi::GetInstance().init();
            bool ok = (ret == ESP_OK);
            g_blufi_started = ok;
            cJSON* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "ok", ok);
            if (!ok) {
                cJSON_AddStringToObject(root, "error", esp_err_to_name(ret));
            }
            return root;
        });
#endif

    mcp_server.AddTool(
        "self.wifi.blufi_status",
        "查询 BluFi 配网状态：是否正在运行、WiFi 是否已连接",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            wifi_ap_record_t ap_info;
            bool connected = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);
            char ssid[33] = "N/A";
            if (connected) {
                snprintf(ssid, sizeof(ssid), "%s", (char*)ap_info.ssid);
            }
            char json[128];
            snprintf(json, sizeof(json),
                     "{\"blufi_active\":%d,\"wifi_connected\":%d,\"ssid\":\"%s\"}",
                     g_blufi_started ? 1 : 0, connected ? 1 : 0, ssid);
            return std::string(json);
        });
}
