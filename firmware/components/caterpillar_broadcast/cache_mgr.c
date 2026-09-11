/*
 * cache_mgr.c — 内容缓存管理器实现
 *
 * 纯 C、依赖 ESP-IDF 的 esp_http_client + LittleFS(SPIFFS)。
 * 下载全部在调用线程同步执行；生产环境中应在 net_task 中调用。
 */
#include "cache_mgr.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>

#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_timer.h>
#include <esp_spiffs.h>
#include <cJSON.h>

static const char *TAG = "cache_mgr";

/* ---- 配置 ---- */
#define CACHE_BASE_PATH       "/spiffs/bc"
#define MANIFEST_FILE          "manifest.json"
#define MAX_CACHE_FILES        16          /* 每包 ~500KB, 16 个 ≈ 8MB */
#define DOWNLOAD_BUF_SIZE      4096
#define MANIFEST_FETCH_TIMEOUT_MS  15000
#define AUDIO_FETCH_TIMEOUT_MS     60000

/* ---- 全局状态 ---- */
static char      g_base_url[128];
static bool      g_initialized = false;
static cache_entry_t g_entries[MAX_CACHE_FILES];
static int       g_entry_count = 0;

/* ---- 内部辅助 ---- */

static void ensure_dir(const char *path) {
    /* LittleFS 不支持 mkdir -p,逐级创建 */
    char tmp[128];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);   /* 已存在会返回 -1,忽略 */
            *p = '/';
        }
    }
}

static int entry_cmp_by_date(const void *a, const void *b) {
    return strcmp(((const cache_entry_t *)b)->date,
                  ((const cache_entry_t *)a)->date);  /* 降序:最新在前 */
}

static void evict_oldest(void) {
    if (g_entry_count < MAX_CACHE_FILES)
        return;

    /* 按日期排序,最旧的排最后（实际是 FIFO 淘汰,非真 LRU） */
    qsort(g_entries, g_entry_count, sizeof(cache_entry_t), entry_cmp_by_date);

    /* 删除最旧的一个 */
    cache_entry_t *victim = &g_entries[g_entry_count - 1];
    char full[128];
    snprintf(full, sizeof(full), "%s%s", CACHE_BASE_PATH, victim->path);
    if (unlink(full) == 0) {
        ESP_LOGI(TAG, "LRU evict: %s (%lu bytes)", full,
                 (unsigned long)victim->size_bytes);
    }
    g_entry_count--;
}

/* ---- HTTP 下载辅助 ---- */

typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} download_buf_t;

static esp_err_t http_write_cb(esp_http_client_event_t *evt) {
    download_buf_t *db = (download_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (db->len + evt->data_len > db->cap) {
            size_t new_cap = db->cap ? db->cap * 2 : DOWNLOAD_BUF_SIZE;
            uint8_t *p = realloc(db->data, new_cap);
            if (!p) return ESP_FAIL;
            db->data = p;
            db->cap  = new_cap;
        }
        memcpy(db->data + db->len, evt->data, evt->data_len);
        db->len += evt->data_len;
    }
    return ESP_OK;
}

static bool http_get(const char *url, download_buf_t *db, int timeout_ms) {
    memset(db, 0, sizeof(*db));
    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_write_cb,
        .user_data = db,
        .timeout_ms = timeout_ms,
        .buffer_size = DOWNLOAD_BUF_SIZE,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "HTTP GET %s → err=%d status=%d", url, err, status);
        free(db->data);
        db->data = NULL;
        db->len = 0;
        return false;
    }
    return true;
}

/* ---- 初始化 ---- */

bool cache_mgr_init(const char *base_url) {
    if (g_initialized) return true;

    if (base_url && base_url[0])
        strncpy(g_base_url, base_url, sizeof(g_base_url) - 1);
    else
        return false;

    /* 挂载 LittleFS 内容分区 */
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "content",
        .max_files = 5,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount LittleFS content partition: %s",
                 esp_err_to_name(ret));
        return false;
    }

    /* 扫描已有缓存文件,重建 g_entries */
    ensure_dir(CACHE_BASE_PATH);
    /* (完整实现应遍历 /spiffs/bc/ 目录;原型版在首次 sync 后自然填充) */

    g_initialized = true;
    ESP_LOGI(TAG, "Cache mgr initialized: base=%s", g_base_url);
    return true;
}

/* ---- 核心:拉取 manifest 并下载音频 ---- */

int cache_mgr_sync(void) {
    if (!g_initialized) return -1;

    /* 1. 获取今天的日期 */
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    char today[11];
    strftime(today, sizeof(today), "%Y-%m-%d", &tm_info);

    /* 2. 下载 manifest */
    char url[256];
    snprintf(url, sizeof(url), "%s/manifest?date=%s", g_base_url, today);
    download_buf_t db;
    if (!http_get(url, &db, MANIFEST_FETCH_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "Manifest fetch failed for %s", today);
        return -1;
    }

    /* 3. 解析 manifest */
    cJSON *root = cJSON_ParseWithLength((const char *)db.data, db.len);
    free(db.data);
    if (!root) {
        ESP_LOGE(TAG, "Manifest parse error");
        return -2;
    }

    cJSON *packages = cJSON_GetObjectItem(root, "packages");
    if (!cJSON_IsArray(packages)) {
        cJSON_Delete(root);
        return -2;
    }

    int downloaded = 0;
    ensure_dir(CACHE_BASE_PATH);

    cJSON *pkg;
    cJSON_ArrayForEach(pkg, packages) {
        cJSON *slot_j = cJSON_GetObjectItem(pkg, "slot");
        cJSON *audio_j = cJSON_GetObjectItem(pkg, "audio");
        if (!cJSON_IsString(slot_j) || !cJSON_IsString(audio_j))
            continue;

        const char *slot = slot_j->valuestring;
        const char *audio = audio_j->valuestring;

        /* 4. 下载音频文件 */
        char audio_url[256];
        snprintf(audio_url, sizeof(audio_url), "%s/audio/%s/%s",
                 g_base_url, today, audio);

        download_buf_t adb;
        if (!http_get(audio_url, &adb, AUDIO_FETCH_TIMEOUT_MS))
            continue;

        /* 5. 写入 LittleFS — 文件名统一用 {slot}.opus，与 cache_mgr_path 一致 */
        char file_path[128];
        snprintf(file_path, sizeof(file_path), "%s/%s/%s.opus",
                 CACHE_BASE_PATH, today, slot);
        ensure_dir(file_path);  /* 创建日期子目录 */

        FILE *fp = fopen(file_path, "wb");
        if (!fp) {
            ESP_LOGE(TAG, "Failed to open %s for writing", file_path);
            free(adb.data);
            continue;
        }
        size_t written = fwrite(adb.data, 1, adb.len, fp);
        fclose(fp);
        free(adb.data);

        if (written != adb.len) {
            ESP_LOGW(TAG, "Truncated write: %s (%u/%u)", file_path,
                     (unsigned)written, (unsigned)adb.len);
            continue;
        }

        /* 6. 更新索引 */
        evict_oldest();  /* 确保有空间 */
        cache_entry_t *e = &g_entries[g_entry_count++];
        strncpy(e->date, today, sizeof(e->date) - 1);
        strncpy(e->slot, slot, sizeof(e->slot) - 1);
        snprintf(e->path, sizeof(e->path), "/%s/%s.opus", today, slot);
        e->size_bytes = (uint32_t)adb.len;
        e->downloaded_at = (uint32_t)now;
        downloaded++;

        ESP_LOGI(TAG, "Cached: %s/%s (%lu bytes)",
                 today, slot, (unsigned long)e->size_bytes);
    }

    cJSON_Delete(root);
    if (downloaded == 0)
        return 1;  /* manifest OK 但没有需要下载的(已缓存) */

    return 0;
}

/* ---- 播报接口 ---- */

bool cache_mgr_has(const char *date_str, const char *slot) {
    for (int i = 0; i < g_entry_count; i++) {
        if (strcmp(g_entries[i].date, date_str) == 0 &&
            strcmp(g_entries[i].slot, slot) == 0)
            return true;
    }
    return false;
}

const char *cache_mgr_path(const char *date_str, const char *slot) {
    static char full[128];
    snprintf(full, sizeof(full), "%s/%s/%s.opus",
             CACHE_BASE_PATH, date_str, slot);
    /* 简单拼接,"/" 可能在 slot 参数里重复,但 LittleFS 容忍 */
    return full;
}

bool cache_mgr_prefetch(const char *date_str) {
    /* 原型: 同步下载明天的包。生产可改为异步(Celery任务队列触发)。 */
    return cache_mgr_sync() == 0;
}

/* ---- 统计 ---- */

void cache_mgr_stats(uint32_t *total_bytes, uint32_t *used_bytes) {
    /* 从 SPIFFS 获取真实分区大小，替代原来硬编码的 8MB */
    size_t spiffs_total = 0, spiffs_used = 0;
    esp_err_t ret = esp_spiffs_info("content", &spiffs_total, &spiffs_used);
    if (total_bytes) {
        *total_bytes = (ret == ESP_OK) ? (uint32_t)spiffs_total : 0;
    }
    if (used_bytes) {
        /* 优先用 SPIFFS 报告的已用量，回退到索引累加 */
        if (ret == ESP_OK && spiffs_used > 0) {
            *used_bytes = (uint32_t)spiffs_used;
        } else {
            uint32_t used = 0;
            for (int i = 0; i < g_entry_count; i++)
                used += g_entries[i].size_bytes;
            *used_bytes = used;
        }
    }
}

int cache_mgr_count(void) {
    return g_entry_count;
}

void cache_mgr_clear(void) {
    for (int i = 0; i < g_entry_count; i++) {
        char full[128];
        snprintf(full, sizeof(full), "%s%s", CACHE_BASE_PATH, g_entries[i].path);
        unlink(full);
    }
    g_entry_count = 0;
    ESP_LOGI(TAG, "Cache cleared");
}
