// 数据更新后台任务
//
// 负责周期性更新屏幕上的动态数据：
// - NTP 时间同步 + 时间跳变保护
// - 时钟/日历更新（每分钟）
// - 天气数据更新（每 10 分钟）
// - 温湿度传感器读取
// - 电池状态 / WiFi 图标更新
// - AI 状态文字更新
// - 备忘闹钟检查

#include "custom_lcd_display.h"

#include <cmath>
#include <cstring>
#include <cJSON.h>
#include <sys/time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

#include "application.h"
#include "board.h"
#include "settings.h"
#include "assets/lang_config.h"
#include "managers/sensor_manager.h"
#include "managers/weather_manager.h"
#include "managers/pomodoro_manager.h"
#include "secret_config.h"
#include <wifi_manager.h>

// 声明状态栏图标（DataUpdateTask 需要更新图标）
LV_IMAGE_DECLARE(ui_img_wifi);
LV_IMAGE_DECLARE(ui_img_wifi_low);
LV_IMAGE_DECLARE(ui_img_wifi_off);
LV_IMAGE_DECLARE(ui_img_battery_full);
LV_IMAGE_DECLARE(ui_img_battery_medium);
LV_IMAGE_DECLARE(ui_img_battery_low);
LV_IMAGE_DECLARE(ui_img_battery_charging);

static const char *TAG = "DataUpdate";

void CustomLcdDisplay::StartDataUpdateTask() {
    // 启用和风天气 API 配置
    WeatherManager::getInstance().setApiConfig(
        WEATHER_API_KEY,
        WEATHER_API_HOST
    );

    // 栈从 16KB 下调到 8KB，给音频/MQTT 留更多 SRAM 余量
    // 优先级保持较低，避免与语音收发实时链路抢占 CPU
    xTaskCreate(DataUpdateTask, "weather_ui_update", 8192, this, 2, &update_task_handle_);
}

void CustomLcdDisplay::DataUpdateTask(void *arg) {
    CustomLcdDisplay *self = (CustomLcdDisplay *)arg;
    bool time_synced = false;

    // NTP 指数退避重试参数
    int ntp_retry_count = 0;
    const int NTP_MAX_RETRIES = 5;            // 最多重试 5 次后放弃
    uint32_t ntp_retry_delay_ms = 1000;       // 初始延迟 1 秒，每次翻倍（1s, 2s, 4s, 8s, 16s）
    uint32_t ntp_last_sync_ms = 0;            // 上次 NTP 同步成功的时间（用于 24 小时校准）
    const uint32_t NTP_RESYNC_INTERVAL = 24 * 60 * 60 * 1000;  // 24 小时（毫秒）

    // 天气更新参数
    bool weather_fetched = false;             // 是否已成功获取过天气
    int weather_retry_count = 0;
    const int WEATHER_MAX_RETRIES = 3;       // 最多重试 3 次
    uint32_t weather_last_sync_ms = 0;
    const uint32_t WEATHER_RESYNC_INTERVAL = 30 * 60 * 1000;  // 30 分钟刷新一次

    // 电池电量变化很慢，降频采样可显著减轻 ADC 和 UI 刷新压力
    const uint32_t BATTERY_POLL_INTERVAL = 10 * 1000;           // 每 10 秒采样一次
    uint32_t last_battery_poll_ms = 0;
    int cached_battery_level = 0;
    bool cached_charging = false, cached_discharging = false;
    bool battery_cached = false;

    // 等待一会让系统启动完成
    vTaskDelay(pdMS_TO_TICKS(3000));
    
    // 记录进入 idle 的时刻，用于"连续 idle 足够久才发网络请求"的保护
    uint32_t idle_since_ms = 0;
    
    // 初始化活动时间（系统启动算一次活动）
    self->last_activity_ms_ = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // 用于备忘闹钟检查的时间信息（在锁外使用）
    struct tm timeinfo;
    
    // 时区设置只需初始化一次，避免每秒 setenv/tzset 带来的系统开销
    setenv("TZ", TIMEZONE_STRING, 1);
    tzset();
    
    while (1) {
        auto& app = Application::GetInstance();
        DeviceState ds = app.GetDeviceState();
        
        // 判断网络是否已连接（必须不是 starting、配网、未知等前期状态）
        bool network_connected = (ds != kDeviceStateStarting && 
                                  ds != kDeviceStateWifiConfiguring &&
                                  ds != kDeviceStateUnknown);
        
        uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        const bool in_audio_session = (ds == kDeviceStateConnecting ||
                                       ds == kDeviceStateListening ||
                                       ds == kDeviceStateSpeaking);

        // ===== 连续 idle 计时 =====
        // NTP / 天气等网络请求必须在"连续 idle 超过 N 秒"后才发，
        // 防止刚开机或刚结束对话就发 HTTPS 请求，和下一次唤醒撞车导致首句卡顿
        const uint32_t IDLE_GUARD_MS = 5000;  // 连续 idle 至少 5 秒才允许网络请求
        if (ds == kDeviceStateIdle) {
            if (idle_since_ms == 0) {
                idle_since_ms = now_ms;
            }
        } else {
            idle_since_ms = 0;  // 非 idle 立刻重置
        }
        bool idle_long_enough = (idle_since_ms > 0 && (now_ms - idle_since_ms >= IDLE_GUARD_MS));
        
        // ===== NTP 时间同步 =====
        // 仅在连续 idle 足够久后同步，避免与 AI 对话抢网络/内存
        if (network_connected && idle_long_enough) {
            bool should_sync = false;
            
            if (!time_synced && ntp_retry_count < NTP_MAX_RETRIES) {
                // 首次同步：未同步且未超过最大重试次数
                should_sync = true;
            } else if (time_synced && ntp_last_sync_ms > 0 && 
                       (now_ms - ntp_last_sync_ms > NTP_RESYNC_INTERVAL)) {
                // 定期校准：已同步但超过 24 小时，重新同步一次
                ESP_LOGI(TAG, "距上次 NTP 同步已超过 24 小时，重新校准...");
                should_sync = true;
            }
            
            if (should_sync) {
                ESP_LOGI(TAG, "同步 NTP 时间 (第 %d 次)...", ntp_retry_count + 1);
                SensorManager::getInstance().syncNtpTime();
                
                // 检查时间是否合理（年份 > 2024 说明同步成功了）
                time_t now_check;
                struct tm check_info;
                time(&now_check);
                localtime_r(&now_check, &check_info);
                if (check_info.tm_year + 1900 >= 2024) {
                    time_synced = true;
                    ntp_retry_count = 0;           // 重置重试计数
                    ntp_retry_delay_ms = 1000;     // 重置退避延迟
                    ntp_last_sync_ms = now_ms;     // 记录同步成功时间
                    self->last_min_ = -1;          // 强制刷新 UI
                    time(&self->last_valid_epoch_);
                    ESP_LOGI(TAG, "NTP 同步成功: %04d-%02d-%02d %02d:%02d",
                             check_info.tm_year + 1900, check_info.tm_mon + 1, check_info.tm_mday,
                             check_info.tm_hour, check_info.tm_min);
                } else {
                    ntp_retry_count++;
                    ESP_LOGW(TAG, "NTP 同步失败（年份=%d），第 %d/%d 次，%d 秒后重试", 
                             check_info.tm_year + 1900, ntp_retry_count, NTP_MAX_RETRIES,
                             (int)(ntp_retry_delay_ms / 1000));
                    // 指数退避等待后再继续循环
                    vTaskDelay(pdMS_TO_TICKS(ntp_retry_delay_ms));
                    ntp_retry_delay_ms *= 2;  // 下次翻倍：1s → 2s → 4s → 8s → 16s
                    if (ntp_retry_delay_ms > 16000) ntp_retry_delay_ms = 16000;
                    
                    if (ntp_retry_count >= NTP_MAX_RETRIES) {
                        ESP_LOGE(TAG, "NTP 同步已失败 %d 次，放弃重试（使用 RTC 时间）", NTP_MAX_RETRIES);
                    }
                }
            }
        }
        
        // ===== 天气更新（启动后自动拉取，之后每 30 分钟刷新）=====
        if (network_connected && idle_long_enough) {
            bool should_update_weather = false;

            if (!weather_fetched && weather_retry_count < WEATHER_MAX_RETRIES) {
                // 首次获取：未获取成功且未超过最大重试次数
                should_update_weather = true;
            } else if (weather_fetched && weather_last_sync_ms > 0 &&
                       (now_ms - weather_last_sync_ms > WEATHER_RESYNC_INTERVAL)) {
                // 定期刷新：已获取成功但超过 30 分钟
                should_update_weather = true;
            }

            if (should_update_weather) {
                ESP_LOGI(TAG, "自动获取天气数据 (第 %d 次)...", weather_retry_count + 1);
                if (WeatherManager::getInstance().update()) {
                    weather_fetched = true;
                    weather_retry_count = 0;
                    weather_last_sync_ms = now_ms;
                    ESP_LOGI(TAG, "天气自动更新成功");
                } else {
                    weather_retry_count++;
                    ESP_LOGW(TAG, "天气获取失败，第 %d/%d 次", weather_retry_count, WEATHER_MAX_RETRIES);
                    if (weather_retry_count >= WEATHER_MAX_RETRIES) {
                        ESP_LOGE(TAG, "天气获取已失败 %d 次，等待 30 分钟后重试", WEATHER_MAX_RETRIES);
                        weather_last_sync_ms = now_ms;  // 30 分钟后再试
                    }
                }
            }
        }
        
        // ===== 时间获取（在锁外也需要用，所以先获取）=====
        time_t now;
        time(&now);
        localtime_r(&now, &timeinfo);
        
        // 记录本轮是否分钟变了（备忘闹钟检查也要用）
        bool minute_changed = (timeinfo.tm_min != self->last_min_);
        
        // ===== UI 更新（每秒）=====
        // 🔑 如果正在显示系统信息滚动，跳过整个 UI 更新块（避免锁竞争）
        if (!self->showing_system_info_) {
            DisplayLockGuard lock(self);
            
            // 时间跳变保护：NTP 同步后，如果系统 epoch 被外部改了（偏差>2小时），
            // 从硬件 RTC 恢复正确时间
            if (time_synced && self->last_valid_epoch_ > 0) {
                long drift = (long)(now - self->last_valid_epoch_);
                // 正常每秒循环 drift ≈ 1s，如果绝对值 > 7200s（2小时），肯定异常
                if (drift < -7200 || drift > 7200) {
                    ESP_LOGW(TAG, "系统时间被篡改（偏差 %ld 秒），从 RTC 恢复", drift);
                    struct tm rtc_tm;
                    SensorManager::getInstance().getRtcTime(&rtc_tm);
                    time_t rtc_epoch = mktime(&rtc_tm);
                    if (rtc_epoch > 1700000000) {
                        struct timeval tv = { .tv_sec = rtc_epoch, .tv_usec = 0 };
                        settimeofday(&tv, NULL);
                        time(&now);
                        localtime_r(&now, &timeinfo);
                        self->last_min_ = -1;
                        minute_changed = true;  // 时间恢复后强制触发
                        ESP_LOGI(TAG, "已从 RTC 恢复: %02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
                    }
                }
                self->last_valid_epoch_ = now;
            }
            
            // 每分钟或强制刷新时更新 UI
            if (minute_changed) {
                char time_buf[16];
                strftime(time_buf, sizeof(time_buf), "%H:%M", &timeinfo);
                if (self->time_label_) lv_label_set_text(self->time_label_, time_buf);
                if (self->music_time_label_) lv_label_set_text(self->music_time_label_, time_buf);
                if (self->pomo_time_label_) lv_label_set_text(self->pomo_time_label_, time_buf);
                if (self->stock_time_label_) lv_label_set_text(self->stock_time_label_, time_buf);

                const char *weeks_en[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
                if (self->day_label_) lv_label_set_text(self->day_label_, weeks_en[timeinfo.tm_wday]);

                char date_buf[8];
                snprintf(date_buf, sizeof(date_buf), "%d", timeinfo.tm_mday);
                if (self->date_num_label_) lv_label_set_text(self->date_num_label_, date_buf);

                self->last_min_ = timeinfo.tm_min;
                ESP_LOGI(TAG, "时间已更新: %s, %s, %d日", time_buf, weeks_en[timeinfo.tm_wday], timeinfo.tm_mday);
            }
        }  // DisplayLockGuard 自动释放

        // ===== 备忘闹钟检查（在锁外执行，避免长时间持锁）=====
        if (minute_changed) {
            if (time_synced) {
                Settings memo_rd("memo", false);
                std::string memo_json = memo_rd.GetString("items", "");
                
                // 🔍 调试：打印当前时间和备忘录 JSON
                char time_buf[16];
                strftime(time_buf, sizeof(time_buf), "%H:%M", &timeinfo);
                ESP_LOGI(TAG, "⏰ 备忘闹钟检查: 当前时间=%s, NVS数据=%s", 
                         time_buf, memo_json.empty() ? "(空)" : memo_json.c_str());
                
                if (!memo_json.empty()) {
                    cJSON *memo_arr = cJSON_Parse(memo_json.c_str());
                    if (memo_arr && cJSON_IsArray(memo_arr)) {
                        bool memo_changed = false;
                        int total_items = cJSON_GetArraySize(memo_arr);
                        ESP_LOGI(TAG, "⏰ 备忘列表共 %d 条", total_items);
                        
                        // 倒序遍历，这样删除不会打乱前面的索引
                        for (int mi = total_items - 1; mi >= 0; mi--) {
                            cJSON *memo_item = cJSON_GetArrayItem(memo_arr, mi);
                            cJSON *mt = cJSON_GetObjectItem(memo_item, "t");
                            cJSON *mc = cJSON_GetObjectItem(memo_item, "c");
                            
                            // 🔍 调试：打印每条备忘的时间
                            if (mt && mt->valuestring) {
                                ESP_LOGI(TAG, "⏰ 检查备忘[%d]: 时间=%s, 内容=%s", 
                                         mi, mt->valuestring, 
                                         (mc && mc->valuestring) ? mc->valuestring : "(空)");
                            }
                            
                            // 只匹配 "HH:MM" 格式（5个字符，中间是冒号）
                            if (mt && mt->valuestring && strlen(mt->valuestring) == 5 
                                && mt->valuestring[2] == ':') {
                                if (strcmp(mt->valuestring, time_buf) == 0) {
                                    const char *memo_text = (mc && mc->valuestring) ? mc->valuestring : "备忘提醒";
                                    char alert_buf[128];
                                    snprintf(alert_buf, sizeof(alert_buf), "备忘提醒: %s %s", mt->valuestring, memo_text);
                                    ESP_LOGI(TAG, "🔔 触发备忘闹钟: %s", alert_buf);
                                    
                                    // 播放提示音 + 屏幕显示提醒
                                    app.Alert("提醒", alert_buf, "happy", Lang::Sounds::OGG_POPUP);

                                    // 触发后从列表中删除这条
                                    cJSON_DeleteItemFromArray(memo_arr, mi);
                                    memo_changed = true;
                                }
                            }
                        }
                        // 如果有条目被删除，写回 NVS 并刷新屏幕
                        if (memo_changed) {
                            char *new_json = cJSON_PrintUnformatted(memo_arr);
                            {
                                Settings memo_wr("memo", true);
                                memo_wr.SetString("items", new_json);
                            }
                            cJSON_free(new_json);
                            self->RefreshMemoDisplay();  // 这个函数会自动获取锁
                            ESP_LOGI(TAG, "✅ 已过期备忘已自动删除");
                        }
                        cJSON_Delete(memo_arr);
                    }
                }
            }
        }

        // ===== 其他 UI 更新（需要重新获取锁）=====
        // 🔑 如果正在显示系统信息滚动，跳过 UI 更新（避免锁竞争）
        if (!self->showing_system_info_) {
            DisplayLockGuard lock(self);
            static uint32_t last_noncritical_ui_update_ms = 0;
            const bool allow_noncritical_update =
                !in_audio_session ||
                last_noncritical_ui_update_ms == 0 ||
                (now_ms - last_noncritical_ui_update_ms >= 5000);

            // 对话期间将非关键 UI/传感器刷新降频到 5 秒一次，避免与语音链路抢占 CPU
            if (allow_noncritical_update) {
                last_noncritical_ui_update_ms = now_ms;

                // 2. 温湿度更新
                SensorData sd = SensorManager::getInstance().getTempHumidity();
                if (sd.valid) {
                    if (fabs(sd.temperature - self->last_temp_) > 0.2f || fabs(sd.humidity - self->last_humi_) > 1.0f) {
                        char buf[32];
                        snprintf(buf, sizeof(buf), "%.1f°C  %.0f%%", sd.temperature, sd.humidity);
                        if (self->sensor_label_) lv_label_set_text(self->sensor_label_, buf);
                        if (self->music_sensor_label_) lv_label_set_text(self->music_sensor_label_, buf);
                        if (self->pomo_sensor_label_) lv_label_set_text(self->pomo_sensor_label_, buf);
                        self->last_temp_ = sd.temperature;
                        self->last_humi_ = sd.humidity;
                    }
                }

                // 3. 天气更新（内容变化时才刷新，避免无效重绘）
                WeatherData wd = WeatherManager::getInstance().getLatestData();
                if (wd.valid && self->weather_label_) {
                    char weather_buf[48];
                    snprintf(weather_buf, sizeof(weather_buf), "%s %s %s°C",
                             wd.city.c_str(), wd.text.c_str(), wd.temp.c_str());
                    static std::string last_weather_text;
                    if (last_weather_text != weather_buf) {
                        lv_label_set_text(self->weather_label_, weather_buf);
                        last_weather_text = weather_buf;
                    }
                }

                // 4. 电池状态更新（采样降频 + 变化更新）
                auto& board = Board::GetInstance();
                if (!battery_cached || last_battery_poll_ms == 0 ||
                    (now_ms - last_battery_poll_ms >= BATTERY_POLL_INTERVAL)) {
                    int level = 0;
                    bool charging = false, discharging = false;
                    if (board.GetBatteryLevel(level, charging, discharging)) {
                        cached_battery_level = level;
                        cached_charging = charging;
                        cached_discharging = discharging;
                        battery_cached = true;
                        last_battery_poll_ms = now_ms;
                    }
                }

                if (battery_cached) {
                    static int last_icon_mode = -1;       // 0=low,1=medium,2=full,3=charging
                    static int last_battery_level = -1;   // 上次显示的电量百分比
                    int icon_mode = 2;
                    if (cached_charging) {
                        icon_mode = 3;
                    } else if (cached_battery_level < 20) {
                        icon_mode = 0;
                    } else if (cached_battery_level < 60) {
                        icon_mode = 1;
                    }

                    if (icon_mode != last_icon_mode) {
                        const void* icon_src = &ui_img_battery_full;
                        if (icon_mode == 3) icon_src = &ui_img_battery_charging;
                        else if (icon_mode == 0) icon_src = &ui_img_battery_low;
                        else if (icon_mode == 1) icon_src = &ui_img_battery_medium;

                        if (self->battery_icon_img_) {
                            lv_image_set_src(self->battery_icon_img_, icon_src);
                        }
                        if (self->music_battery_icon_img_) {
                            lv_image_set_src(self->music_battery_icon_img_, icon_src);
                        }
                        if (self->pomo_battery_icon_img_) {
                            lv_image_set_src(self->pomo_battery_icon_img_, icon_src);
                        }
                        if (self->stock_battery_icon_img_) {
                            lv_image_set_src(self->stock_battery_icon_img_, icon_src);
                        }
                        last_icon_mode = icon_mode;
                    }

                    if (self->battery_pct_label_ && cached_battery_level != last_battery_level) {
                        char bat_buf[16];
                        snprintf(bat_buf, sizeof(bat_buf), "%d%%", cached_battery_level);
                        lv_label_set_text(self->battery_pct_label_, bat_buf);
                        if (self->music_battery_pct_label_) lv_label_set_text(self->music_battery_pct_label_, bat_buf);
                        if (self->pomo_battery_pct_label_) lv_label_set_text(self->pomo_battery_pct_label_, bat_buf);
                        if (self->stock_battery_pct_label_) lv_label_set_text(self->stock_battery_pct_label_, bat_buf);
                        last_battery_level = cached_battery_level;
                    }

                    // 低电量提醒（对齐原版行为）：
                    // - 放电且低于 20% 时显示弹窗并播一次提示音
                    // - 回升到 25% 及以上（或进入充电）后隐藏，避免 19/20% 抖动反复闪烁
                    static bool low_battery_popup_visible = false;
                    const bool should_show_low_battery = (!cached_charging && cached_discharging && cached_battery_level < 20);
                    const bool should_hide_low_battery = (cached_charging || !cached_discharging || cached_battery_level >= 25);
                    if (self->low_battery_popup_) {
                        if (!low_battery_popup_visible && should_show_low_battery) {
                            lv_obj_remove_flag(self->low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                            app.PlaySound(Lang::Sounds::OGG_LOW_BATTERY);
                            low_battery_popup_visible = true;
                        } else if (low_battery_popup_visible && should_hide_low_battery) {
                            lv_obj_add_flag(self->low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                            low_battery_popup_visible = false;
                        }
                    }
                }

                // 5. WiFi 图标更新（根据真实 Wi-Fi 状态变化时才更新）
                static enum class WifiIconState {
                    Unknown,
                    Disconnected,
                    Connected,
                    Configuring,
                } last_wifi_state = WifiIconState::Unknown;

                auto& wifi_manager = WifiManager::GetInstance();
                WifiIconState wifi_state = WifiIconState::Disconnected;
                if (wifi_manager.IsConfigMode()) {
                    wifi_state = WifiIconState::Configuring;
                } else if (wifi_manager.IsConnected()) {
                    wifi_state = WifiIconState::Connected;
                }

                if (wifi_state != last_wifi_state) {
                    const void* wifi_src = &ui_img_wifi_off;
                    if (wifi_state == WifiIconState::Connected) {
                        wifi_src = &ui_img_wifi;
                    } else if (wifi_state == WifiIconState::Configuring) {
                        wifi_src = &ui_img_wifi_low;
                    }
                    if (self->wifi_icon_img_) {
                        lv_image_set_src(self->wifi_icon_img_, wifi_src);
                    }
                    if (self->music_wifi_icon_img_) {
                        lv_image_set_src(self->music_wifi_icon_img_, wifi_src);
                    }
                    if (self->pomo_wifi_icon_img_) {
                        lv_image_set_src(self->pomo_wifi_icon_img_, wifi_src);
                    }
                    if (self->stock_wifi_icon_img_) {
                        lv_image_set_src(self->stock_wifi_icon_img_, wifi_src);
                    }
                    last_wifi_state = wifi_state;
                }
            }

            // 7. 股票数据更新（腾讯财经 API）
            static uint32_t stock_last_sync_ms = 0;
            static bool stock_was_visible = false;
            static bool was_trading = false;
            const uint32_t STOCK_RESYNC_TRADING = 30 * 1000;     // 交易时间 30 秒
            const uint32_t STOCK_RESYNC_NON_TRADING = 30 * 60 * 1000;  // 非交易时间 30 分钟

            // 切换到股票页时立即获取一次数据（不等间隔）
            bool stock_visible = self->IsStockMode();
            if (stock_visible && !stock_was_visible) {
                stock_last_sync_ms = 0;  // 重置，立即触发获取
            }
            stock_was_visible = stock_visible;

            // 强制刷新（长按BOOT按钮触发）
            if (self->force_stock_refresh_) {
                stock_last_sync_ms = 0;
                self->force_stock_refresh_ = false;
            }

            // 判断是否在交易时间（9:30-11:30, 13:00-15:30）
            bool is_trading = false;
            if (time_synced) {
                int hour = timeinfo.tm_hour;
                int min = timeinfo.tm_min;
                int weekday = timeinfo.tm_wday;
                if (weekday >= 1 && weekday <= 5) {  // 周一到周五
                    int t = hour * 60 + min;
                    if ((t >= 570 && t <= 710) || (t >= 780 && t <= 900)) {  // 9:30-11:30, 13:00-15:00
                        is_trading = true;
                    }
                }

                // 开盘时刻（9:30 或 13:00）立即刷新
                if (is_trading && !was_trading) {
                    stock_last_sync_ms = 0;
                }
                was_trading = is_trading;
            }

            uint32_t stock_interval = is_trading ? STOCK_RESYNC_TRADING : STOCK_RESYNC_NON_TRADING;

            // 调试日志：显示股票数据获取的各个条件状态
            static uint32_t last_stock_debug_ms = 0;
            if (now_ms - last_stock_debug_ms >= 10000) {  // 每10秒打印一次
                ESP_LOGI(TAG, "股票条件: net=%d idle=%d time_sync=%d trading=%d interval=%dms since=%dms",
                         network_connected, idle_long_enough, time_synced, is_trading,
                         stock_interval, (int)(now_ms - stock_last_sync_ms));
                last_stock_debug_ms = now_ms;
            }

            if (network_connected && idle_long_enough && time_synced && self->IsStockMode() &&
                (stock_last_sync_ms == 0 || now_ms - stock_last_sync_ms >= stock_interval)) {

                // 腾讯财经 API 请求
                // 指数：sh000001(上证), sz399001(深证), sz399006(创业板), sh000688(科创50)
                // 自选股：sh601012, sh601991, sh512760, sz159583, sz515880, sz001309
                const char* url = "http://qt.gtimg.cn/q=sh000001,sz399001,sz399006,sh000688,sh601012,sh601991,sh512760,sz159611,sz159583,sz300394,sz002558,sz001309";

                esp_http_client_config_t config = {
                    .url = url,
                    .timeout_ms = 5000,
                };
                esp_http_client_handle_t client = esp_http_client_init(&config);
                if (client) {
                    esp_err_t err = esp_http_client_open(client, 0);
                    if (err == ESP_OK) {
                        int content_length = esp_http_client_fetch_headers(client);
                        int status = esp_http_client_get_status_code(client);
                        // 腾讯财经 API 可能不返回 Content-Length，使用 chunked 传输
                        // 分配固定缓冲区（32KB 足够 10 个指数/股票数据，实际约 12-15KB）
                        if (content_length <= 0) content_length = 32768;
                        ESP_LOGI(TAG, "股票API: status=%d, content_length=%d", status, content_length);
                        if (status == 200) {
                            char* response = (char*)malloc(content_length + 1);
                            if (response) {
                                // 分块读取响应，直到没有更多数据
                                int total_read = 0;
                                int bytes;
                                while (total_read < content_length) {
                                    bytes = esp_http_client_read(client, response + total_read, content_length - total_read);
                                    if (bytes <= 0) break;
                                    total_read += bytes;
                                }
                                response[total_read] = '\0';

                                // 调试：打印前200字节看返回内容
                                ESP_LOGI(TAG, "股票API响应(%d字节): %.200s", total_read, response);

                                // 解析腾讯财经数据
                                // 格式：v_sh000001="1~上证指数~000001~3350.20~3322.66~..."
                                // 字段：[2]代码 [3]当前价 [4]昨收 [5]今开 [31]涨跌额 [32]涨跌幅 [33]最高 [34]最低 [37]成交额
                                //
                                // 注意：中文名是 GBK 编码，双字节中可能包含 0x7E（即 '~'），
                                //       所以不能用 strchr 逐个跳过 '~'，否则会误匹配中文内的字节。
                                //       策略：先找到纯 ASCII 的股票代码 "~XXXXXX~"，再从代码后计数 tilde。

                                static float sh_index = 0, sh_change = 0;
                                static float sz_index = 0, sz_change = 0;
                                static float cy_index = 0, cy_change = 0;
                                static float kc50_index = 0, kc50_change = 0;

                                // 通用提取函数：在 response 中找 var_name 开头的条目，
                                // 然后定位 ~code~ 并从代码后提取第 field_offset 个字段
                                auto extract_field = [](const char* resp, const char* var_name,
                                                       const char* code, int field_offset) -> float {
                                    char pattern[32];
                                    snprintf(pattern, sizeof(pattern), "%s=\"", var_name);
                                    char* entry = strstr(resp, pattern);
                                    if (!entry) return 0;

                                    // 在该条目内找 ~CODE~ （纯 ASCII，不会误匹配 GBK）
                                    char code_pat[16];
                                    snprintf(code_pat, sizeof(code_pat), "~%s~", code);
                                    char* code_pos = strstr(entry, code_pat);
                                    if (!code_pos) return 0;

                                    // 从代码后面开始，代码本身是 field [2]，
                                    // ~CODE~ 后面已经是 field [3]，所以要跳 field_offset - 3 个 tilde
                                    char* p = code_pos + strlen(code_pat);
                                    int tilde_count = field_offset - 3;
                                    if (tilde_count < 0) tilde_count = 0;
                                    for (int i = 0; i < tilde_count && p; i++) {
                                        p = strchr(p, '~');
                                        if (p) p++;
                                    }
                                    return p ? atof(p) : 0;
                                };

                                // 提取指数
                                sh_index  = extract_field(response, "v_sh000001", "000001", 3);
                                sh_change = extract_field(response, "v_sh000001", "000001", 32);
                                sz_index  = extract_field(response, "v_sz399001", "399001", 3);
                                sz_change = extract_field(response, "v_sz399001", "399001", 32);
                                cy_index  = extract_field(response, "v_sz399006", "399006", 3);
                                cy_change = extract_field(response, "v_sz399006", "399006", 32);
                                kc50_index  = extract_field(response, "v_sh000688", "000688", 3);
                                kc50_change = extract_field(response, "v_sh000688", "000688", 32);

                                // 更新指数标签
                                DisplayLockGuard lock(self);
                                ESP_LOGI(TAG, "指数: 上证=%.2f(%.2f%%) 深证=%.2f(%.2f%%) 创业板=%.2f(%.2f%%) 科创50=%.2f(%.2f%%)",
                                         sh_index, sh_change, sz_index, sz_change, cy_index, cy_change, kc50_index, kc50_change);
                                self->UpdateStockIndexLabels(sh_index, sh_change, sz_index, sz_change, cy_index, cy_change, kc50_index, kc50_change);

                                // 解析自选股数据并更新
                                struct StockParse {
                                    const char* var_name;  // "v_sh601012"
                                    const char* code;      // "601012"
                                };
                                static const StockParse stocks[] = {
                                    {"v_sh601012", "601012"},
                                    {"v_sh601991", "601991"},
                                    {"v_sh512760", "512760"},
                                    {"v_sz159611", "159611"},
                                    {"v_sz159583", "159583"},
                                    {"v_sz300394", "300394"},
                                    {"v_sz002558", "002558"},
                                    {"v_sz001309", "001309"},
                                };

                                for (const auto& sp : stocks) {
                                    float price     = extract_field(response, sp.var_name, sp.code, 3);
                                    float pre_close = extract_field(response, sp.var_name, sp.code, 4);
                                    float open      = extract_field(response, sp.var_name, sp.code, 5);
                                    float high      = extract_field(response, sp.var_name, sp.code, 33);
                                    float low       = extract_field(response, sp.var_name, sp.code, 34);
                                    float amount    = extract_field(response, sp.var_name, sp.code, 37);
                                    float change_pct= extract_field(response, sp.var_name, sp.code, 32);
                                    float turnover  = extract_field(response, sp.var_name, sp.code, 38);

                                    if (price <= 0) continue;  // 未找到该股票，跳过

                                    // 构造 code 格式："sh600519" / "sz000001"
                                    char full_code[16];
                                    snprintf(full_code, sizeof(full_code), "%.2s%s",
                                             sp.var_name + 2, sp.code);  // "sh" or "sz" + code

                                    // 找到对应股票索引并更新
                                    for (int i = 0; i < self->stock_list_count_; i++) {
                                        if (strcmp(self->stock_list_[i].code, full_code) == 0) {
                                            ESP_LOGI(TAG, "股票[%d] %s: 价格=%.2f 涨跌=%.2f%%", i, full_code, price, change_pct);
                                            self->UpdateStockLabels(i, price, change_pct, open, pre_close, high, low, amount, turnover);
                                            break;
                                        }
                                    }
                                }

                                // === 报警检测 ===
                                // 只在空闲状态才报警（避免打断 AI 对话或音乐）
                                if (ds == kDeviceStateIdle) {
                                    const float STOCK_ALERT_THRESHOLD = 3.0f;    // 个股：±3%
                                    const float INDEX_ALERT_THRESHOLD = 1.0f;    // 指数：±1%
                                    const uint32_t ALERT_COOLDOWN_MS = 10 * 60 * 1000;  // 10分钟冷却
                                    static uint32_t last_stock_alert_ms[10] = {};
                                    static uint32_t last_index_alert_ms = 0;
                                    static float last_alert_prices[10] = {};  // 个股报警时的价格基准

                                    bool any_alert = false;

                                    // 个股报警：当前价格 vs 上次报警时的价格变化超 ±3%
                                    for (int i = 0; i < self->stock_list_count_ && i < 10; i++) {
                                        float cur_price = self->stock_list_[i].price;
                                        float ref_price = last_alert_prices[i];
                                        if (ref_price > 0 && cur_price > 0) {
                                            float change = fabsf((cur_price - ref_price) / ref_price * 100.0f);
                                            if (change >= STOCK_ALERT_THRESHOLD &&
                                                (now_ms - last_stock_alert_ms[i] > ALERT_COOLDOWN_MS)) {
                                                ESP_LOGW(TAG, "报警：%s 价格 %.2f→%.2f 变化 %.1f%%",
                                                         self->stock_list_[i].name, ref_price, cur_price, change);
                                                any_alert = true;
                                                last_stock_alert_ms[i] = now_ms;
                                                last_alert_prices[i] = cur_price;  // 更新基准价
                                                // 反色闪烁该股票标签
                                                self->FlashAlertLabel(i);
                                            }
                                        } else {
                                            // 首次获取数据，设置基准价
                                            last_alert_prices[i] = cur_price;
                                        }
                                    }

                                    // 指数报警：涨跌幅超 ±1%
                                    if ((fabsf(sh_change) >= INDEX_ALERT_THRESHOLD ||
                                         fabsf(sz_change) >= INDEX_ALERT_THRESHOLD ||
                                         fabsf(cy_change) >= INDEX_ALERT_THRESHOLD) &&
                                        (now_ms - last_index_alert_ms > ALERT_COOLDOWN_MS)) {
                                        ESP_LOGW(TAG, "报警：指数 上证%.2f%% 深证%.2f%% 创业板%.2f%%",
                                                 sh_change, sz_change, cy_change);
                                        any_alert = true;
                                        last_index_alert_ms = now_ms;
                                    }

                                    // 播放提示音
                                    if (any_alert) {
                                        app.PlaySound(Lang::Sounds::OGG_POPUP);
                                    }
                                }

                                free(response);
                            }
                        }
                        esp_http_client_close(client);
                    }
                    esp_http_client_cleanup(client);
                    stock_last_sync_ms = now_ms;
                    // 数据获取完成，隐藏刷新提示
                    self->ShowRefreshIndicator(false);
                }
            }

            // 6. AI 状态更新
            static DeviceState last_ds = kDeviceStateUnknown;
            if (ds != last_ds) {
                // AI 状态发生变化（对话、聆听等），视为用户活动
                if (ds == kDeviceStateListening || ds == kDeviceStateSpeaking || 
                    ds == kDeviceStateConnecting) {
                    self->NotifyUserActivity();
                }

                // 更新左侧表情区域（显示当前状态简称）
                const char* emotion_text = "待命";
                const char* status_text = "";
                switch (ds) {
                    case kDeviceStateConnecting:      emotion_text = "连接"; status_text = "连接中..."; break;
                    case kDeviceStateListening:       emotion_text = "聆听"; status_text = "聆听中..."; break;
                    case kDeviceStateSpeaking:        emotion_text = "说话"; break;  // 对话文字由 SetChatMessage 更新
                    case kDeviceStateStarting:        emotion_text = "启动"; status_text = "启动中..."; break;
                    case kDeviceStateWifiConfiguring: emotion_text = "配网"; break;   // 详细文案由 Alert() -> SetChatMessage 设置
                    case kDeviceStateUpgrading:       emotion_text = "升级"; status_text = "升级中..."; break;
                    case kDeviceStateActivating:      emotion_text = "激活"; break;   // 详细文案由 Alert() -> SetChatMessage 设置
                    case kDeviceStateFatalError:      emotion_text = "错误"; status_text = "发生错误"; break;
                    case kDeviceStateIdle:            emotion_text = "待命"; break;   // 空闲时表情由 SetEmotion 管理
                    default: break;
                }
                if (self->emotion_label_) {
                    lv_label_set_text(self->emotion_label_, emotion_text);
                }
                if (self->music_emotion_label_) {
                    lv_label_set_text(self->music_emotion_label_, emotion_text);
                }
                // 番茄钟运行中时，番茄钟页面的 AI 卡片不被普通状态变化覆盖
                // 只在空闲时才让番茄钟页面跟随设备状态
                auto& pomo_inst = PomodoroManager::getInstance();
                bool pomo_running = (pomo_inst.getState() != PomodoroManager::IDLE);

                if (!pomo_running && self->pomo_emotion_label_) {
                    lv_label_set_text(self->pomo_emotion_label_, emotion_text);
                }
                // 非说话/配网/激活状态时更新右侧文字（这些状态由 Alert/SetChatMessage 管理详细信息）
                if (ds != kDeviceStateSpeaking && ds != kDeviceStateWifiConfiguring &&
                    ds != kDeviceStateActivating && self->chat_status_label_ && strlen(status_text) > 0) {
                    // 状态短文案（如“聆听中...”）必须强制不滚动，避免继承上一条长文本动画
                    self->SetShowingSystemInfo(false);
                    lv_anim_delete(self->chat_status_label_, nullptr);
                    lv_label_set_long_mode(self->chat_status_label_, LV_LABEL_LONG_WRAP);
                    lv_obj_align(self->chat_status_label_, LV_ALIGN_LEFT_MID, 64 + 20, 0);
                    lv_label_set_text(self->chat_status_label_, status_text);
                }
                if (ds != kDeviceStateSpeaking && ds != kDeviceStateWifiConfiguring &&
                    ds != kDeviceStateActivating && self->music_chat_status_label_ && strlen(status_text) > 0) {
                    lv_label_set_long_mode(self->music_chat_status_label_, LV_LABEL_LONG_WRAP);
                    lv_label_set_text(self->music_chat_status_label_, status_text);
                }
                if (!pomo_running && ds != kDeviceStateSpeaking && ds != kDeviceStateWifiConfiguring &&
                    ds != kDeviceStateActivating && self->pomo_chat_status_label_ && strlen(status_text) > 0) {
                    lv_label_set_long_mode(self->pomo_chat_status_label_, LV_LABEL_LONG_WRAP);
                    lv_label_set_text(self->pomo_chat_status_label_, status_text);
                }
                last_ds = ds;
            }
        }  // DisplayLockGuard 自动释放

        // ===== 番茄钟 UI 刷新 =====
        // 番茄钟运行时，每秒更新倒计时显示和进度条
        {
            auto& pomo = PomodoroManager::getInstance();
            auto pomo_state = pomo.getState();
            if (pomo_state != PomodoroManager::IDLE && self->pomo_countdown_label_) {
                // 计算进度（千分比）
                int total = pomo.getTotalSeconds();
                int remaining = pomo.getRemainingSeconds();
                int progress = 0;
                if (total > 0) {
                    progress = ((total - remaining) * 1000) / total;
                }

                // 状态文字
                const char* state_text = "倒计时中";
                if (pomo_state == PomodoroManager::PAUSED) {
                    state_text = "已暂停";
                }

                // 设定信息
                char info_buf[64];
                snprintf(info_buf, sizeof(info_buf), "共 %d 分钟", pomo.getMinutes());

                // 更新 UI
                self->UpdatePomodoroDisplay(
                    state_text,
                    pomo.getRemainingTimeStr().c_str(),
                    progress,
                    info_buf
                );

                // 番茄钟运行中时，覆盖底部 AI 卡的显示
                // 只在 AI 不说话时更新（说话时由 SetChatMessage 管理）
                if (ds != kDeviceStateSpeaking) {
                    DisplayLockGuard pomo_lock(self);
                    if (self->pomo_emotion_label_) {
                        const char* pomo_emoji = (pomo_state == PomodoroManager::PAUSED) ? "暂停" : "专注";
                        lv_label_set_text(self->pomo_emotion_label_, pomo_emoji);
                    }
                    if (self->pomo_chat_status_label_) {
                        char pomo_status_buf[64];
                        if (pomo_state == PomodoroManager::PAUSED) {
                            snprintf(pomo_status_buf, sizeof(pomo_status_buf), "已暂停，说“继续番茄钟”可恢复");
                        } else {
                            snprintf(pomo_status_buf, sizeof(pomo_status_buf), "白噪音播放中，专注进行中");
                        }
                        lv_label_set_long_mode(self->pomo_chat_status_label_, LV_LABEL_LONG_WRAP);
                        lv_label_set_text(self->pomo_chat_status_label_, pomo_status_buf);
                    }
                }
            }
        }

        // ===== 省电模式检测 =====
        // 5 分钟无活动（无按钮、无 AI 对话）时进入省电模式，降低刷新频率
        // 注意：必须重新取当前时间，因为 NotifyUserActivity() 可能在本轮循环中
        // 被 AI 状态变化触发过，如果用循环开头的 now_ms 会导致 uint32 溢出
        {
            uint32_t check_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            uint32_t activity_ms = self->last_activity_ms_;
            if (!self->power_saving_ && activity_ms > 0 && check_ms >= activity_ms) {
                if (check_ms - activity_ms > self->IDLE_TIMEOUT_MS) {
                    self->power_saving_ = true;
                    ESP_LOGI(TAG, "⚡ 5 分钟无活动，进入省电模式（刷新间隔 %d 秒 → %d 秒）",
                             self->NORMAL_REFRESH_MS / 1000, self->SAVING_REFRESH_MS / 1000);
                }
            }
        }
        
        // 动态刷新间隔：正常 1 秒，省电 5 秒
        int delay_ms = self->power_saving_ ? self->SAVING_REFRESH_MS : self->NORMAL_REFRESH_MS;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}
