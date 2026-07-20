// 股票桌面摆件 — 卡片版
// 布局 (400x300, 1-bit RLCD)：
// ┌────────────────────────────────────┐
// │ 10:39              [WiFi 🔋 85%] │ 状态栏
// ├────────────────────────────────────┤
// │ ┌──────────────────────────────┐  │ 指数卡片
// │ │ 上证 3792.10 +0.74%          │  │
// │ └──────────────────────────────┘  │
// ├──────────────┬─────────────────────┤
// │ ┌──────────┐ │ ┌─────────────────┐ │
// │ │▶贵州茅台 │ │ │ 贵州茅台 600519 │ │
// │ │ 平安银行 │ │ │ 1253.00 +1.25%  │ │
// │ │ 中国平安 │ │ │ 今开:1240 昨收.. │ │
// │ │ 宁德时代 │ │ │ 最高:1260 最低.. │ │
// │ │ 比亚迪   │ │ │ 成交额:12.5亿    │ │
// │ └──────────┘ │ │ 涨跌额:+15.00   │ │
// │              │ └─────────────────┘ │
// ├──────────────┴─────────────────────┤
// │ ┌──────────────────────────────┐  │ 创业板卡片
// │ │ 创业板 3428.63 +0.59%        │  │
// │ └──────────────────────────────┘  │
// └────────────────────────────────────┘

#include "custom_lcd_display.h"
#include <esp_log.h>
#include <cstdio>
#include <cstring>

LV_FONT_DECLARE(font_puhui_16_4);

// 声明状态栏图标
LV_IMAGE_DECLARE(ui_img_wifi);
LV_IMAGE_DECLARE(ui_img_wifi_low);
LV_IMAGE_DECLARE(ui_img_wifi_off);
LV_IMAGE_DECLARE(ui_img_battery_full);
LV_IMAGE_DECLARE(ui_img_battery_medium);
LV_IMAGE_DECLARE(ui_img_battery_low);
LV_IMAGE_DECLARE(ui_img_battery_charging);

static const char *TAG = "StockUI";

// 创建卡片容器（白底黑边圆角）
static lv_obj_t* create_card(lv_obj_t *parent, int x, int y, int w, int h,
                              lv_color_t bg_color, lv_color_t border_color, int border_w, int radius) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, bg_color, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, border_w, 0);
    lv_obj_set_style_border_color(card, border_color, 0);
    lv_obj_set_style_radius(card, radius, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

void CustomLcdDisplay::SetupStockUI() {
    DisplayLockGuard lock(this);
    lv_obj_t *root = lv_screen_active();
    const lv_font_t *f = &font_puhui_16_4;

    // ===== 页面容器 =====
    stock_page_ = lv_obj_create(root);
    lv_obj_set_size(stock_page_, 400, 300);
    lv_obj_set_pos(stock_page_, 0, 0);
    lv_obj_set_style_bg_opa(stock_page_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(stock_page_, 0, 0);
    lv_obj_set_style_pad_all(stock_page_, 0, 0);
    lv_obj_remove_flag(stock_page_, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *screen = stock_page_;

    // ===== 状态栏（右上角白底胶囊，和天气页完全一致）=====
    lv_obj_t *status_bar = lv_obj_create(screen);
    lv_obj_set_size(status_bar, 115, 28);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(status_bar, lv_color_white(), 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_radius(status_bar, 14, 0);
    lv_obj_align(status_bar, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_obj_set_style_pad_all(status_bar, 0, 0);
    lv_obj_set_style_pad_left(status_bar, 8, 0);
    lv_obj_set_style_pad_right(status_bar, 8, 0);
    lv_obj_remove_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status_bar, 5, 0);

    stock_wifi_icon_img_ = lv_image_create(status_bar);
    lv_image_set_src(stock_wifi_icon_img_, &ui_img_wifi_off);

    stock_battery_icon_img_ = lv_image_create(status_bar);
    lv_image_set_src(stock_battery_icon_img_, &ui_img_battery_full);

    stock_battery_pct_label_ = lv_label_create(status_bar);
    lv_obj_set_style_text_font(stock_battery_pct_label_, f, 0);
    lv_obj_set_style_text_color(stock_battery_pct_label_, lv_color_black(), 0);
    lv_label_set_text(stock_battery_pct_label_, "--%");

    // ===== 左上角时钟（白字，和天气页一致）=====
    stock_time_label_ = lv_label_create(screen);
    lv_obj_set_style_text_font(stock_time_label_, f, 0);
    lv_obj_set_style_text_color(stock_time_label_, lv_color_white(), 0);
    lv_obj_align(stock_time_label_, LV_ALIGN_TOP_LEFT, 10, 8);
    lv_label_set_text(stock_time_label_, "--:--");

    // ===== 布局常量 =====
    const int pad = 8;
    const int gap = 6;

    // ===== 指数卡片（顶部，全宽）=====
    lv_obj_t *index_card = create_card(screen, pad, 36, 400 - pad * 2, 26,
                                        lv_color_white(), lv_color_black(), 2, 10);
    stock_index_top_label_ = lv_label_create(index_card);
    lv_obj_set_style_text_font(stock_index_top_label_, f, 0);
    lv_obj_set_style_text_color(stock_index_top_label_, lv_color_black(), 0);
    lv_obj_center(stock_index_top_label_);
    lv_label_set_text(stock_index_top_label_, "上证 ----  深证 ----");

    // ===== 左侧股票列表卡片（加宽到 140px，6只股票）=====
    int list_y = 36 + 26 + gap;  // y=68
    int list_h = 300 - list_y - 32 - gap;  // 底部留32给一行指数
    int list_w = 160;
    lv_obj_t *list_card = create_card(screen, pad, list_y, list_w, list_h,
                                       lv_color_white(), lv_color_black(), 2, 10);

    AddStock("sh601012", "隆基绿能");
    AddStock("sh601991", "大唐发电");
    AddStock("sh512760", "芯片ETF");
    AddStock("sz159611", "电力ETF");
    AddStock("sz159583", "通信ETF");
    AddStock("sz300394", "天孚通信");
    AddStock("sz002558", "巨人网络");
    AddStock("sz001309", "德明利");

    for (int i = 0; i < stock_list_count_ && i < STOCK_MAX_COUNT; i++) {
        // 名称标签（左对齐）
        lv_obj_t *l = lv_label_create(list_card);
        lv_obj_set_style_text_font(l, f, 0);
        lv_obj_set_pos(l, 6, 6 + i * 20);
        lv_obj_set_width(l, 90);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(l, lv_color_black(), 0);

        // 价格标签（右对齐）
        lv_obj_t *p = lv_label_create(list_card);
        lv_obj_set_style_text_font(p, f, 0);
        lv_obj_set_style_text_color(p, lv_color_black(), 0);
        lv_obj_align(p, LV_ALIGN_TOP_RIGHT, -6, 6 + i * 20);
        lv_label_set_text(p, "--");

        if (i == 0) {
            lv_obj_set_style_text_color(l, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(l, lv_color_black(), 0);
            lv_obj_set_style_pad_left(l, 2, 0);
            lv_obj_set_style_pad_right(l, 2, 0);
            lv_obj_set_style_text_color(p, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(p, lv_color_black(), 0);
        }
        lv_label_set_text(l, stock_list_[i].name);
        stock_list_items_[i] = l;
        stock_price_items_[i] = p;
    }

    // ===== 右侧股票详情卡片 =====
    int detail_x = pad + list_w + gap;
    int detail_w = 400 - pad * 2 - list_w - gap;
    lv_obj_t *detail_card = create_card(screen, detail_x, list_y, detail_w, list_h,
                                          lv_color_white(), lv_color_black(), 2, 10);

    stock_name_label_ = lv_label_create(detail_card);
    lv_obj_set_style_text_font(stock_name_label_, f, 0);
    lv_obj_set_style_text_color(stock_name_label_, lv_color_black(), 0);
    lv_obj_set_pos(stock_name_label_, 8, 8);
    if (stock_list_count_ > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%s %s", stock_list_[0].name, stock_list_[0].code);
        lv_label_set_text(stock_name_label_, buf);
    } else {
        lv_label_set_text(stock_name_label_, "未添加股票");
    }

    stock_price_label_ = lv_label_create(detail_card);
    lv_obj_set_style_text_font(stock_price_label_, f, 0);
    lv_obj_set_style_text_color(stock_price_label_, lv_color_black(), 0);
    lv_obj_set_pos(stock_price_label_, 8, 28);
    lv_label_set_text(stock_price_label_, "----.--  --.--%");

    stock_info_label_ = lv_label_create(detail_card);
    lv_obj_set_style_text_font(stock_info_label_, f, 0);
    lv_obj_set_style_text_color(stock_info_label_, lv_color_black(), 0);
    lv_obj_set_pos(stock_info_label_, 8, 48);
    lv_label_set_text(stock_info_label_, "今开:---- 昨收:----");

    stock_high_low_label_ = lv_label_create(detail_card);
    lv_obj_set_style_text_font(stock_high_low_label_, f, 0);
    lv_obj_set_style_text_color(stock_high_low_label_, lv_color_black(), 0);
    lv_obj_set_pos(stock_high_low_label_, 8, 68);
    lv_label_set_text(stock_high_low_label_, "最高:---- 最低:----");

    stock_amount_label_ = lv_label_create(detail_card);
    lv_obj_set_style_text_font(stock_amount_label_, f, 0);
    lv_obj_set_style_text_color(stock_amount_label_, lv_color_black(), 0);
    lv_obj_set_pos(stock_amount_label_, 8, 88);
    lv_label_set_text(stock_amount_label_, "成交额:--亿");

    stock_change_label_ = lv_label_create(detail_card);
    lv_obj_set_style_text_font(stock_change_label_, f, 0);
    lv_obj_set_style_text_color(stock_change_label_, lv_color_black(), 0);
    lv_obj_set_pos(stock_change_label_, 8, 108);
    lv_label_set_text(stock_change_label_, "涨跌额:--+.--");

    stock_amplitude_label_ = lv_label_create(detail_card);
    lv_obj_set_style_text_font(stock_amplitude_label_, f, 0);
    lv_obj_set_style_text_color(stock_amplitude_label_, lv_color_black(), 0);
    lv_obj_set_pos(stock_amplitude_label_, 8, 128);
    lv_label_set_text(stock_amplitude_label_, "振幅:--.--%");

    stock_turnover_label_ = lv_label_create(detail_card);
    lv_obj_set_style_text_font(stock_turnover_label_, f, 0);
    lv_obj_set_style_text_color(stock_turnover_label_, lv_color_black(), 0);
    lv_obj_set_pos(stock_turnover_label_, 8, 148);
    lv_label_set_text(stock_turnover_label_, "换手:--.--%");

    // ===== 底部指数卡片（创业板 + 科创50，一行）=====
    int bot_y = 300 - 32;
    lv_obj_t *bot_card = create_card(screen, pad, bot_y, 400 - pad * 2, 28,
                                      lv_color_white(), lv_color_black(), 2, 10);

    stock_index_bottom_label_ = lv_label_create(bot_card);
    lv_obj_set_style_text_font(stock_index_bottom_label_, f, 0);
    lv_obj_set_style_text_color(stock_index_bottom_label_, lv_color_black(), 0);
    lv_obj_align(stock_index_bottom_label_, LV_ALIGN_LEFT_MID, 6, 0);
    lv_label_set_text(stock_index_bottom_label_, "创业板 ----  --.%  科创50 ----  --.%");
    lv_label_set_long_mode(stock_index_bottom_label_, LV_LABEL_LONG_DOT);

    // ===== 基类占位控件（防止空指针崩溃，和天气页一致）=====
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, 1, 1);
    lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);

    network_label_ = lv_label_create(screen);
    lv_label_set_text(network_label_, "");
    lv_obj_add_flag(network_label_, LV_OBJ_FLAG_HIDDEN);

    battery_label_ = lv_label_create(screen);
    lv_label_set_text(battery_label_, "");
    lv_obj_add_flag(battery_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(screen);
    lv_label_set_text(status_label_, "");
    lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);

    notification_label_ = lv_label_create(screen);
    lv_label_set_text(notification_label_, "");
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    mute_label_ = lv_label_create(screen);
    lv_label_set_text(mute_label_, "");
    lv_obj_add_flag(mute_label_, LV_OBJ_FLAG_HIDDEN);

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, 320, 42);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(low_battery_popup_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(low_battery_popup_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(low_battery_popup_, 2, 0);
    lv_obj_set_style_border_color(low_battery_popup_, lv_color_black(), 0);
    lv_obj_set_style_radius(low_battery_popup_, 12, 0);
    lv_obj_set_style_pad_all(low_battery_popup_, 6, 0);
    lv_obj_remove_flag(low_battery_popup_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_obj_set_style_text_font(low_battery_label_, f, 0);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(low_battery_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(low_battery_label_, 300);
    lv_obj_center(low_battery_label_);
    lv_label_set_text(low_battery_label_, "电量低，请尽快充电");

    emoji_label_ = lv_label_create(screen);
    lv_label_set_text(emoji_label_, "");
    lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);

    emoji_image_ = lv_img_create(screen);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

    chat_message_label_ = stock_price_label_;  // 让基类方法能找到一个 label

    ESP_LOGI(TAG, "股票页创建完成（卡片版）");
}
