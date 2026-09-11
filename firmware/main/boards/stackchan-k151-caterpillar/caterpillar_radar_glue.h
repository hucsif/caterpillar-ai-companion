#pragma once

#include <cstdint>
#include <functional>

class McpServer;

/* ---- 初始化 ---- */

// 初始化雷达 UART + 解析器 + 状态机 + 播报调度 + 内容缓存，启动 radar_task
void caterpillar_radar_init(void);

// 注册 MCP 工具：self.radar.get_status, self.radar.night_mode
void caterpillar_radar_register_mcp_tools(McpServer& mcp_server);

/* ---- 夜间模式回调（由板级实现） ---- */

// 设备进入夜间模式：熄屏、舵机释放扭矩、LED 灭。板级注册此回调。
using caterpillar_night_mode_fn = std::function<void(bool)>;
void caterpillar_set_night_mode_callback(caterpillar_night_mode_fn cb);

/* ---- 内容缓存 URL 设置 ---- */

// 设置内容服务器地址（用于 cache_mgr 和 caterpillar_http 下载）
void caterpillar_set_server_url(const char* url);

/* ---- 转头追踪回调（R60ABD1 DP5 x/y/z → 舵机 yaw+pitch） ---- */
using caterpillar_head_angle_fn = std::function<void(int16_t yaw_deg, int16_t pitch_deg)>;
void caterpillar_set_head_angle_callback(caterpillar_head_angle_fn cb);

/* ---- 获取最近人体方位 ---- */
// 返回 yaw（水平角），无人返回 INT16_MIN（pitch 也会被忽略）
int16_t caterpillar_get_nearest_person_yaw(void);
// 从 z 坐标估算 pitch（抬头/低头角度）：160cm→45°(平视), 80cm→70°(低头)
int16_t caterpillar_z_to_pitch(void);
