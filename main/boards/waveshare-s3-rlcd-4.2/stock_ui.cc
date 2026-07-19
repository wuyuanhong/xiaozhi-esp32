// 股票桌面摆件 — 纯文字版
// 顶部指数栏 + 左侧股票列表 + 右侧详情 + 底部创业板指

#include "custom_lcd_display.h"
#include <esp_log.h>
#include <cstdio>
#include <cstring>

LV_FONT_DECLARE(font_puhui_16_4);

static const char *TAG = "StockUI";

void CustomLcdDisplay::SetupStockUI() {
    DisplayLockGuard lock(this);
    lv_obj_t *r = lv_screen_active();
    const lv_font_t *f = &font_puhui_16_4;

    // 创建容器
    stock_page_ = lv_obj_create(r);
    lv_obj_set_size(stock_page_, 400, 300);
    lv_obj_set_pos(stock_page_, 0, 0);
    lv_obj_set_style_bg_opa(stock_page_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(stock_page_, 0, 0);
    lv_obj_set_style_pad_all(stock_page_, 0, 0);
    lv_obj_remove_flag(stock_page_, LV_OBJ_FLAG_SCROLLABLE);

    // ===== 顶部指数栏 =====
    stock_index_top_label_ = lv_label_create(stock_page_);
    lv_obj_set_style_text_font(stock_index_top_label_, f, 0);
    lv_obj_set_style_text_color(stock_index_top_label_, lv_color_white(), 0);
    lv_obj_set_pos(stock_index_top_label_, 4, 4);
    lv_label_set_text(stock_index_top_label_, "上证 ----.- --.-%  深证 -----.- --.-%");

    // ===== 左侧自选股列表 =====
    AddStock("sh600519", "贵州茅台");
    AddStock("sz000001", "平安银行");
    AddStock("sh601318", "中国平安");
    AddStock("sz300750", "宁德时代");
    AddStock("sz002594", "比亚迪");

    for (int i = 0; i < stock_list_count_ && i < STOCK_MAX_COUNT; i++) {
        lv_obj_t *l = lv_label_create(stock_page_);
        lv_obj_set_style_text_font(l, f, 0);
        lv_obj_set_pos(l, 4, 26 + i * 40);
        if (i == 0) {
            lv_obj_set_style_text_color(l, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(l, lv_color_white(), 0);
            lv_obj_set_style_pad_left(l, 2, 0);
            lv_obj_set_style_pad_right(l, 2, 0);
        } else {
            lv_obj_set_style_text_color(l, lv_color_white(), 0);
        }
        char buf[32];
        snprintf(buf, sizeof(buf), "%s  --", stock_list_[i].name);
        lv_label_set_text(l, buf);
        stock_list_items_[i] = l;
    }

    // ===== 右侧股票详情（默认显示第一只股票）=====
    stock_name_label_ = lv_label_create(stock_page_);
    lv_obj_set_style_text_font(stock_name_label_, f, 0);
    lv_obj_set_style_text_color(stock_name_label_, lv_color_white(), 0);
    lv_obj_set_pos(stock_name_label_, 96, 26);
    if (stock_list_count_ > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%s %s", stock_list_[0].name, stock_list_[0].code);
        lv_label_set_text(stock_name_label_, buf);
    } else {
        lv_label_set_text(stock_name_label_, "未添加股票");
    }

    stock_price_label_ = lv_label_create(stock_page_);
    lv_obj_set_style_text_font(stock_price_label_, f, 0);
    lv_obj_set_style_text_color(stock_price_label_, lv_color_white(), 0);
    lv_obj_set_pos(stock_price_label_, 96, 46);
    lv_label_set_text(stock_price_label_, "----.-- --.-.--%");

    stock_info_label_ = lv_label_create(stock_page_);
    lv_obj_set_style_text_font(stock_info_label_, f, 0);
    lv_obj_set_style_text_color(stock_info_label_, lv_color_white(), 0);
    lv_obj_set_pos(stock_info_label_, 96, 68);
    lv_label_set_text(stock_info_label_, "今开:----  昨收:----");

    stock_high_low_label_ = lv_label_create(stock_page_);
    lv_obj_set_style_text_font(stock_high_low_label_, f, 0);
    lv_obj_set_style_text_color(stock_high_low_label_, lv_color_white(), 0);
    lv_obj_set_pos(stock_high_low_label_, 96, 88);
    lv_label_set_text(stock_high_low_label_, "最高:----  最低:----");

    stock_amount_label_ = lv_label_create(stock_page_);
    lv_obj_set_style_text_font(stock_amount_label_, f, 0);
    lv_obj_set_style_text_color(stock_amount_label_, lv_color_white(), 0);
    lv_obj_set_pos(stock_amount_label_, 96, 108);
    lv_label_set_text(stock_amount_label_, "成交额:--亿");

    stock_change_label_ = lv_label_create(stock_page_);
    lv_obj_set_style_text_font(stock_change_label_, f, 0);
    lv_obj_set_style_text_color(stock_change_label_, lv_color_white(), 0);
    lv_obj_set_pos(stock_change_label_, 96, 128);
    lv_label_set_text(stock_change_label_, "涨跌额:--+.--");

    // ===== 底部创业板指 =====
    stock_index_bottom_label_ = lv_label_create(stock_page_);
    lv_obj_set_style_text_font(stock_index_bottom_label_, f, 0);
    lv_obj_set_style_text_color(stock_index_bottom_label_, lv_color_white(), 0);
    lv_obj_set_pos(stock_index_bottom_label_, 96, 150);
    lv_label_set_text(stock_index_bottom_label_, "创业板 -----.- --.-.%");

    ESP_LOGI(TAG, "股票页创建完成");
}
