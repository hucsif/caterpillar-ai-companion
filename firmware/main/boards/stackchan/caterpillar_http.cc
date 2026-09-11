#include "caterpillar_http.h"

#include <cstring>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "application.h"
#include "mcp_server.h"
#include "protocol.h"
#include "settings.h"

static const char* TAG = "caterpillar_http";
static char g_server_url[128] = "";

static void _auto_play_timer_cb(void* /*arg*/) {
    ESP_LOGI(TAG, "Auto-play morning broadcast");
    caterpillar_http_play("morning", "2026-09-07");
}

void caterpillar_http_init(const char* server_url) {
    // 优先用传入的 URL；如果为空，尝试从 NVS websocket.url 推导
    if (server_url && server_url[0]) {
        strncpy(g_server_url, server_url, sizeof(g_server_url) - 1);
        g_server_url[sizeof(g_server_url) - 1] = '\0';
    } else {
        // 从 websocket.url NVS 提取 IP:端口（例 ws://192.168.3.43:8000/xiaozhi/v1/ → http://192.168.3.43:8000）
        Settings settings("websocket", false);
        std::string ws_url = settings.GetString("url");
        if (!ws_url.empty()) {
            // ws_url like "ws://192.168.3.43:8000/xiaozhi/v1/"
            // extract "192.168.3.43:8000"
            size_t proto_end = ws_url.find("://");
            if (proto_end != std::string::npos) {
                size_t host_start = proto_end + 3;
                size_t host_end = ws_url.find('/', host_start);
                std::string host_port = ws_url.substr(host_start, host_end - host_start);
                // 内容服务器跟 xiaozhi-server 同主机不同端口：xiaozhi=8000, 内容=8001
                snprintf(g_server_url, sizeof(g_server_url), "http://%s:8001", host_port.c_str());
            }
        }
    }

    // 开机 30 秒后自动播一次晨间播报 — 暂时禁用，改走豆包实时语音
    // esp_timer_create_args_t timer_args = {};
    // timer_args.callback = _auto_play_timer_cb;
    // timer_args.name = "auto_play";
    // esp_timer_handle_t auto_play_timer;
    // esp_timer_create(&timer_args, &auto_play_timer);
    // esp_timer_start_once(auto_play_timer, 30 * 1000 * 1000);  // 30 seconds
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

// ---------- 获取当前日期字符串 ----------

static void get_date_str(char* out, size_t out_len) {
    time_t now = time(nullptr);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    strftime(out, out_len, "%Y-%m-%d", &tm_info);
}

// ---------- 公共接口 ----------

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

    char url[256];
    snprintf(url, sizeof(url), "%s/audio/%s/%s.opus", g_server_url, date, type);
    ESP_LOGI(TAG, "Fetching: %s", url);

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
