#pragma once

class McpServer;

// 如果当前没有 WiFi 连接，自动启动 BluFi BLE 配网。
// 家长用"毛毛虫智感"小程序或 ESP BluFi App 扫码连接后发送 SSID/密码。
void caterpillar_blufi_init(void);

// 注册 MCP 工具: self.wifi.blufi_start, self.wifi.blufi_status
void caterpillar_blufi_register_mcp_tools(McpServer& mcp_server);
