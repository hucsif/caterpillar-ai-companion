#pragma once

class McpServer;

// 初始化 HTTP 播报模块
// server_url 形如 "http://192.168.94.130:8000"
void caterpillar_http_init(const char* server_url);

// 注册 MCP 工具: caterpillar.broadcast.play
void caterpillar_register_mcp_tools(McpServer& mcp_server);

// 触发播报: 从服务器下载 type.opus (morning/evening) 并播放
// date_str 如 "2026-09-07", 传 NULL 用今天的日期
// 返回 true 表示下载和播放成功开始
bool caterpillar_http_play(const char* type, const char* date_str);
