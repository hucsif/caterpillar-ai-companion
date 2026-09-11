#pragma once

class McpServer;

// 初始化雷达 UART + 解析器 + 状态机 + 播报调度，启动 radar_task
void caterpillar_radar_init(void);

// 注册 MCP 工具：self.radar.get_status
void caterpillar_radar_register_mcp_tools(McpServer& mcp_server);
