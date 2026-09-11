/*
 * caterpillar_http.cc — 毛毛虫播报 HTTP 下载 + 缓存播放
 *
 * 改造要点（2026-07-24）：
 *   1. 新增 play_from_cache()：优先从 LittleFS 本地缓存读取 opus 播放（断网可用）
 *   2. caterpillar_http_play() 改为"缓存优先"：命中→本地播放，miss→HTTP 下载
 *   3. URL 推导改用 caterpillar_config 统一模块，消除重复解析
 *   4. caterpillar_http_play_url() URL 判断改用 strstr，不再靠首字母 'h'
 */
#include "caterpillar_http.h"

#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include <esp_http_client.h>
#include <esp_log.h>

#include "application.h"
#include "mcp_server.h"
#include "protocol.h"

#include "caterpillar_config.h"
#include "cache_mgr.h"

static const char* TAG = "caterpillar_http";
static char g_server_url[128] = "";

void caterpillar_http_init(const char* server_url) {
    /* 优先用传入的 URL；为空则从统一配置模块推导 */
    if (server_url && server_url[0]) {
        strncpy(g_server_url, server_url, sizeof(g_server_url) - 1);
        g_server_url[sizeof(g_server_url) - 1] = '\0';
    } else {
        caterpillar_endpoints_t ep;
        if (caterpillar_config_init(&ep)) {
            strncpy(g_server_url, ep.server_url, sizeof(g_server_url) - 1);
            g_server_url[sizeof(g_server_url) - 1] = '\0';
        } else {
            ESP_LOGW(TAG, "Server URL not configured (websocket.url missing)");
        }
    }
}

// ---------- HTTP 下载 ----------

static esp_err_t _http_write_cb(esp_http_client_event_t* evt) {
    auto* buf = static_cast<std::vector<uint8_t>*>(evt->user_data);
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        buf->insert(buf->end(), (uint8_t*)evt->data, (uint8_t*)evt->data + evt->data_len);
    }
    return ESP_OK;
}

static bool download_file(const char* url, std::vector<uint8_t>& out) {
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.event_handler = _http_write_cb;
    cfg.user_data = &out;
    cfg.timeout_ms = 30000;
    cfg.buffer_size = 4096;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "HTTP download failed: %s, status=%d, err=%d", url, status, err);
        return false;
    }
    ESP_LOGI(TAG, "Downloaded %s: %u bytes", url, (unsigned)out.size());
    return !out.empty();
}

// ---------- Opus 帧解析并推送播放 ----------

static void play_opus_frames(const uint8_t* data, size_t len) {
    auto& audio = Application::GetInstance().GetAudioService();

    size_t offset = 0;
    int frame_count = 0;
    while (offset + 2 <= len) {
        // 读取 2 字节大端帧长度
        uint16_t frame_len = ((uint16_t)data[offset] << 8) | data[offset + 1];
        offset += 2;
        if (frame_len == 0 || offset + frame_len > len) break;

        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = 16000;
        packet->frame_duration = 60;
        packet->timestamp = 0;
        packet->payload.assign(data + offset, data + offset + frame_len);

        audio.PushPacketToDecodeQueue(std::move(packet), /*wait=*/true);

        offset += frame_len;
        frame_count++;
    }
    ESP_LOGI(TAG, "Pushed %d Opus frames to playback queue", frame_count);
}

// ---------- 从本地缓存文件播放（新增） ----------

static bool play_from_cache(const char* type, const char* date_str) {
    const char* path = cache_mgr_path(date_str, type);
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGD(TAG, "Cache file not found: %s", path);
        return false;
    }

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (len <= 0) {
        fclose(fp);
        ESP_LOGW(TAG, "Empty cache file: %s", path);
        return false;
    }

    std::vector<uint8_t> data(len);
    size_t read = fread(data.data(), 1, len, fp);
    fclose(fp);

    if (read != (size_t)len) {
        ESP_LOGW(TAG, "Truncated cache read: %s (%u/%ld)", path, (unsigned)read, len);
        return false;
    }

    ESP_LOGI(TAG, "Playing from cache: %s (%ld bytes)", path, len);
    play_opus_frames(data.data(), data.size());
    return true;
}

// ---------- 获取当前日期字符串 ----------

static void get_date_str(char* out, size_t out_len) {
    time_t now = time(nullptr);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    strftime(out, out_len, "%Y-%m-%d", &tm_info);
}

// ---------- 公共接口 ----------

bool caterpillar_http_play_url(const char* url) {
    if (!url || !url[0]) {
        ESP_LOGE(TAG, "Empty URL");
        return false;
    }
    /* 用 strstr 判断是否为完整 URL，替代原来 url[0]=='h' 的脆弱判断 */
    const char* final_url;
    char full_buf[256];
    if (strstr(url, "://") != nullptr) {
        final_url = url;
    } else {
        if (g_server_url[0] == '\0') {
            ESP_LOGE(TAG, "Server URL not configured");
            return false;
        }
        snprintf(full_buf, sizeof(full_buf), "%s/audio/%s", g_server_url, url);
        final_url = full_buf;
    }
    ESP_LOGI(TAG, "Fetching: %s", final_url);
    std::vector<uint8_t> data;
    if (!download_file(final_url, data)) return false;
    play_opus_frames(data.data(), data.size());
    return true;
}

bool caterpillar_http_play(const char* type, const char* date_str) {
    if (g_server_url[0] == '\0') {
        ESP_LOGE(TAG, "Server URL not configured");
        return false;
    }

    char date[16];
    if (date_str && date_str[0]) {
        strncpy(date, date_str, sizeof(date) - 1);
    } else {
        get_date_str(date, sizeof(date));
    }
    date[sizeof(date) - 1] = '\0';

    /* 1. 优先本地缓存（断网可用，零延迟） */
    if (cache_mgr_has(date, type)) {
        if (play_from_cache(type, date)) {
            return true;
        }
        ESP_LOGW(TAG, "Cache hit but read failed, falling back to HTTP");
    }

    /* 2. 缓存未命中 → 即时下载播放 */
    char url[256];
    snprintf(url, sizeof(url), "%s/audio/%s/%s.opus", g_server_url, date, type);
    ESP_LOGI(TAG, "Cache miss, fetching: %s", url);

    std::vector<uint8_t> data;
    if (!download_file(url, data)) {
        return false;
    }

    play_opus_frames(data.data(), data.size());
    return true;
}

// ---------- MCP 工具注册 ----------

void caterpillar_register_mcp_tools(McpServer& mcp_server) {
    mcp_server.AddTool(
        "caterpillar.broadcast.play",
        "Trigger caterpillar broadcast playback: download an Opus audio file "
        "from the content server and play it on the device speaker. "
        "type: 'morning' or 'evening'. "
        "date (optional): YYYY-MM-DD date string, defaults to today.",
        PropertyList({Property("type", kPropertyTypeString),
                      Property("date", kPropertyTypeString, std::string())}),
        [](const PropertyList& props) -> ReturnValue {
            std::string type = props["type"].value<std::string>();
            std::string date_str;
            const auto& date_prop = props["date"];
            if (date_prop.was_provided()) {
                date_str = date_prop.value<std::string>();
            }
            bool ok = caterpillar_http_play(type.c_str(),
                                             date_str.empty() ? nullptr : date_str.c_str());
            cJSON* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "ok", ok);
            cJSON_AddStringToObject(root, "type", type.c_str());
            return root;
        });
}
