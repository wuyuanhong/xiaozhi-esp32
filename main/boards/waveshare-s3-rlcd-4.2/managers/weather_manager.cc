#include "weather_manager.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "zlib.h"
#include <string.h>
#include <algorithm>
#include <cctype>

// 默认城市配置（不依赖 secret_config.h 的路径，直接定义在此）
#ifndef DEFAULT_CITY_NAME
#define DEFAULT_CITY_NAME   "衢州"
#endif
#ifndef DEFAULT_CITY_LAT
#define DEFAULT_CITY_LAT    "28.94"
#endif
#ifndef DEFAULT_CITY_LON
#define DEFAULT_CITY_LON    "118.87"
#endif

static const char *TAG = "WeatherManager";

// HTTP 响应缓冲区（分配在 SPIRAM 上，避免占用宝贵的内部 RAM）
static char* response_buffer = NULL;
static int response_len = 0;
static const int RESPONSE_BUFFER_SIZE = 8192;

// GZIP 解压缓冲区（和风天气 API 默认返回 gzip 压缩数据）
static char* decompressed_buffer = NULL;
static const int DECOMPRESSED_BUFFER_SIZE = 8192;

esp_err_t WeatherManager::http_event_handler(esp_http_client_event_t *evt) {
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (response_buffer && response_len + evt->data_len < RESPONSE_BUFFER_SIZE - 1) {
                memcpy(response_buffer + response_len, evt->data, evt->data_len);
                response_len += evt->data_len;
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

WeatherManager::WeatherManager() {
    response_buffer = (char*)heap_caps_malloc(RESPONSE_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    decompressed_buffer = (char*)heap_caps_malloc(DECOMPRESSED_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
}

WeatherManager& WeatherManager::getInstance() {
    static WeatherManager instance;
    return instance;
}

void WeatherManager::setApiConfig(const char* key, const char* host) {
    api_key_ = key;
    api_host_ = host;
}

bool WeatherManager::updateFromExternal(const std::string& city,
                                        const std::string& weather_text,
                                        const std::string& temperature,
                                        const std::string& update_time) {
    if (city.empty() || weather_text.empty() || temperature.empty()) {
        ESP_LOGW(TAG, "外部天气数据无效：city/text/temp 不能为空");
        return false;
    }

    latest_data_.city = city;
    latest_data_.text = weather_text;
    latest_data_.temp = temperature;
    latest_data_.update_time = update_time.empty() ? "mcp" : update_time;
    latest_data_.valid = true;

    ESP_LOGI(TAG, "天气已由外部写入: %s %s %s°C",
             latest_data_.city.c_str(),
             latest_data_.text.c_str(),
             latest_data_.temp.c_str());
    return true;
}

// GZIP 安全解压（和风天气 API 返回 gzip 格式）
static bool decompress_gzip_safe(const uint8_t* src, int src_len, char* dst, int dst_max_len, int* out_len) {
    if (src_len < 18 || src[0] != 0x1f || src[1] != 0x8b) return false;
    z_stream strm = {};
    strm.next_in = (Bytef*)src;
    strm.avail_in = src_len;
    strm.next_out = (Bytef*)dst;
    strm.avail_out = dst_max_len - 1;
    if (inflateInit2(&strm, 15 + 16) != Z_OK) return false;
    int ret = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);
    if (ret != Z_STREAM_END && ret != Z_OK) return false;
    *out_len = dst_max_len - 1 - strm.avail_out;
    dst[*out_len] = '\0';
    return true;
}

bool WeatherManager::update() {
    if (!response_buffer || api_key_.empty() || api_host_.empty()) {
        ESP_LOGW(TAG, "天气 API 未配置或缓冲区未分配");
        return false;
    }

    // 使用 secret_config.h 中配置的默认城市坐标
    double lat = atof(DEFAULT_CITY_LAT);
    double lon = atof(DEFAULT_CITY_LON);
    std::string city_name = DEFAULT_CITY_NAME;

    ESP_LOGI(TAG, "使用配置城市: %s (%.2f, %.2f)", city_name.c_str(), lat, lon);

    // 第二步：获取实时天气数据
    response_len = 0;
    memset(response_buffer, 0, RESPONSE_BUFFER_SIZE);
    char weather_url[512];
    snprintf(weather_url, sizeof(weather_url), 
             "https://%s/v7/weather/now?location=%.2f,%.2f&key=%s&lang=zh", 
             api_host_.c_str(), lon, lat, api_key_.c_str());

    ESP_LOGI(TAG, "获取天气数据...");
    esp_http_client_config_t weather_config = {};
    weather_config.url = weather_url;
    weather_config.event_handler = http_event_handler;
    weather_config.timeout_ms = 15000;
    weather_config.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&weather_config);
    
    esp_http_client_set_header(client, "Host", api_host_.c_str());
    esp_http_client_set_header(client, "User-Agent", "ESP32-Weather-Station");
    esp_http_client_set_header(client, "Accept-Encoding", "gzip");
    
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    bool success = false;

    if (err == ESP_OK && status_code == 200 && response_len > 0) {
        int d_len = 0;
        const char* final_json = NULL;
        if (decompress_gzip_safe((uint8_t*)response_buffer, response_len, 
                                  decompressed_buffer, DECOMPRESSED_BUFFER_SIZE, &d_len)) {
            final_json = decompressed_buffer;
        } else {
            response_buffer[response_len] = '\0';
            final_json = response_buffer;
        }

        if (final_json) {
            parseWeatherJson(final_json);
            latest_data_.city = city_name;
            success = latest_data_.valid;
        }
    } else {
        ESP_LOGE(TAG, "天气请求失败 (err=%d, status=%d)", err, status_code);
    }
    esp_http_client_cleanup(client);
    return success;
}

void WeatherManager::parseWeatherJson(const char* json_data) {
    cJSON *root = cJSON_Parse(json_data);
    if (!root) return;
    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (code && strcmp(code->valuestring, "200") == 0) {
        cJSON *now = cJSON_GetObjectItem(root, "now");
        if (now) {
            latest_data_.temp = cJSON_GetObjectItem(now, "temp")->valuestring;
            latest_data_.text = cJSON_GetObjectItem(now, "text")->valuestring;
            latest_data_.valid = true;
            ESP_LOGI(TAG, "天气更新成功: %s°C, %s", latest_data_.temp.c_str(), latest_data_.text.c_str());
        }
    }
    cJSON_Delete(root);
}
