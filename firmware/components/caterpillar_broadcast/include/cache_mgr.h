/*
 * cache_mgr.h — 内容缓存管理器
 *
 * 保证晨间/晚间播报"断网也能播"的关键模块。
 *
 * 工作流程:
 *   1. 每天 04:00-06:00 随机错峰，HTTPS拉取 manifest.json
 *   2. 按 manifest 下载 morning.opus / evening.opus 到 LittleFS
 *   3. 超过16个文件时 LRU 淘汰最旧的
 *   4. broadcast_sched 触发播报时，优先从缓存读取，缓存未命中才即时下载
 *
 * 分区: content (SPIFFS, 512KB), 存于 /spiffs/bc/ 下
 * 容量: 每天 ~224KB(Opus双包), 512KB可存2天
 *
 * 依赖: esp_http_client, LittleFS, cJSON
 * 文件: stackchan-mcp/firmware/components/caterpillar_broadcast/
 * 创建: 2026-07-23
 */

#ifndef CACHE_MGR_H
#define CACHE_MGR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 缓存条目 */
typedef struct {
    char     date[11];       /* "YYYY-MM-DD" */
    char     slot[8];        /* "morning" / "evening" */
    char     path[64];       /* 相对路径,如 "/bc/2026-09-07/morning.opus" */
    uint32_t size_bytes;
    uint32_t downloaded_at;  /* Unix 时间戳,用于 LRU */
} cache_entry_t;

/* ---- 生命周期 ---- */

/* 初始化:挂载 LittleFS 分区,扫描已有文件,启动每日下载定时器 */
bool cache_mgr_init(const char *base_url);

/* ---- 播报调度器接口 ---- */

/* 检查某个包是否已缓存。返回 true 表示 path 有效可直接播放 */
bool cache_mgr_has(const char *date_str, const char *slot);

/* 获取缓存文件的绝对路径（如 "/spiffs/bc/2026-09-07/morning.opus"）。
 * 调用前应先检查 cache_mgr_has()。返回的指针在下一次 cache_mgr 操作前有效。 */
const char *cache_mgr_path(const char *date_str, const char *slot);

/* 如果连接可用,主动预取明天的包。返回 true 表示已触发下载。
 * 下载是异步的(在 net_task 中执行),结果下次 cache_mgr_has() 可见。 */
bool cache_mgr_prefetch(const char *date_str);

/* ---- 管理与统计 ---- */

/* 手动触发一次 manifest 拉取+下载(用于配网后首次同步)。
 * 返回 0=成功, <0=网络错误, >0=无更新 */
int  cache_mgr_sync(void);

/* 返回缓存分区总空间/已用空间(字节) */
void cache_mgr_stats(uint32_t *total_bytes, uint32_t *used_bytes);

/* 返回当前缓存的包数量 */
int  cache_mgr_count(void);

/* 手动清理所有缓存 */
void cache_mgr_clear(void);

#ifdef __cplusplus
}
#endif
#endif /* CACHE_MGR_H */
