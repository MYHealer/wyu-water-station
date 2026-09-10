/*
 * ui.c - DuduClock 天气主页 (LVGL)
 * 精简布局，居中，深色主题，240x320
 */

#include "ui.h"
#include "display.h"
#include "fonts.h"
#include "config.h"
#include "user_config.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = TAG_APP;

static lv_obj_t *scr;
static lv_obj_t *lbl_city, *lbl_air, *lbl_wea;
static lv_obj_t *lbl_hhmm, *lbl_sec;
static lv_obj_t *lbl_date, *lbl_week;
static lv_obj_t *lbl_temp, *bar_temp;
static lv_obj_t *lbl_humi, *bar_humi;
static lv_obj_t *lbl_scroll;
static lv_obj_t *lbl_dorm, *lbl_water, *lbl_elec;
static int scroll_idx = 0;
static char scroll_texts[5][64];

/* 颜色 */
#define C_BG  lv_color_black()
#define C_FG  lv_color_white()
#define C_ORG lv_color_make(0xFF,0x9F,0x0A)
#define C_RED lv_color_make(0xFF,0x45,0x3A)
#define C_GRN lv_color_make(0x30,0xD1,0x58)
#define C_BLU lv_color_make(0x64,0xD2,0xFF)
#define C_GRY lv_color_make(0x48,0x48,0x4A)
#define C_DK  lv_color_make(0x1C,0x1C,0x1E)
#define C_LN  lv_color_make(0x33,0x33,0x33)

#define W 240
#define CW 220
#define CX 10

/* 分割线 */
static void line(int y) {
    lv_obj_t *l = lv_obj_create(scr);
    lv_obj_set_size(l, W, 1);
    lv_obj_set_pos(l, 0, y);
    lv_obj_set_style_bg_color(l, C_LN, 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(l, 0, 0);
    lv_obj_set_style_radius(l, 0, 0);
    lv_obj_set_style_pad_all(l, 0, 0);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_SCROLLABLE);
}

/* 圆角卡片 */
static lv_obj_t *card(int y, int h) {
    lv_obj_t *c = lv_obj_create(scr);
    lv_obj_set_size(c, CW, h);
    lv_obj_set_pos(c, CX, y);
    lv_obj_set_style_bg_color(c, C_DK, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 12, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static void scroll_cb(lv_timer_t *t) {
    (void)t;
    if (scroll_texts[scroll_idx][0])
        lv_label_set_text(lbl_scroll, scroll_texts[scroll_idx]);
    scroll_idx = (scroll_idx + 1) % 5;
}

static void tick_cb(lv_timer_t *t) { (void)t; ui_update_time(); }

void ui_weather_page_init(void)
{
    ESP_LOGI(TAG, "UI init");
    if (!display_lvgl_lock(pdMS_TO_TICKS(1000))) return;

    scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, W, 320);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* === 天气卡片 y=5, h=45 === */
    lv_obj_t *wc = card(5, 45);
    lbl_city = lv_label_create(wc);
    lv_obj_set_style_text_font(lbl_city, &lv_font_cn_24, 0);
    lv_obj_set_style_text_color(lbl_city, C_ORG, 0);
    lv_label_set_text(lbl_city, "");
    lv_obj_align(lbl_city, LV_ALIGN_LEFT_MID, 10, 0);

    lbl_air = lv_label_create(wc);
    lv_obj_set_style_text_font(lbl_air, &lv_font_cn_24, 0);
    lv_obj_set_style_text_color(lbl_air, C_FG, 0);
    lv_obj_set_style_bg_color(lbl_air, C_GRN, 0);
    lv_obj_set_style_bg_opa(lbl_air, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl_air, 6, 0);
    lv_obj_set_style_pad_hor(lbl_air, 6, 0);
    lv_obj_set_style_pad_ver(lbl_air, 2, 0);
    lv_label_set_text(lbl_air, "");
    lv_obj_align(lbl_air, LV_ALIGN_CENTER, -10, 0);

    lbl_wea = lv_label_create(wc);
    lv_obj_set_style_text_font(lbl_wea, &lv_font_cn_24, 0);
    lv_obj_set_style_text_color(lbl_wea, C_FG, 0);
    lv_label_set_text(lbl_wea, "");
    lv_obj_align(lbl_wea, LV_ALIGN_RIGHT_MID, -10, 0);

    /* === 轮播 y=55 === */
    lbl_scroll = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_scroll, &lv_font_cn_24, 0);
    lv_obj_set_style_text_color(lbl_scroll, C_GRY, 0);
    lv_obj_set_width(lbl_scroll, CW);
    lv_label_set_long_mode(lbl_scroll, LV_LABEL_LONG_DOT);
    lv_label_set_text(lbl_scroll, "");
    lv_obj_align(lbl_scroll, LV_ALIGN_TOP_MID, 0, 55);

    line(82);

    /* === 时钟卡片 y=90, h=80 === */
    lv_obj_t *cc = card(90, 80);

    lbl_hhmm = lv_label_create(cc);
    lv_obj_set_style_text_font(lbl_hhmm, &lv_font_num_64, 0);
    lv_obj_set_style_text_color(lbl_hhmm, C_FG, 0);
    lv_label_set_text(lbl_hhmm, "00:00");
    lv_obj_align(lbl_hhmm, LV_ALIGN_LEFT_MID, 12, 0);

    lbl_sec = lv_label_create(cc);
    lv_obj_set_style_text_font(lbl_sec, &lv_font_num_32, 0);
    lv_obj_set_style_text_color(lbl_sec, C_RED, 0);
    lv_label_set_text(lbl_sec, "00");
    lv_obj_align(lbl_sec, LV_ALIGN_RIGHT_MID, -12, 0);

    line(178);

    /* === 日期 y=185 === */
    lbl_date = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_date, &lv_font_cn_24, 0);
    lv_obj_set_style_text_color(lbl_date, C_FG, 0);
    lv_label_set_text(lbl_date, "");
    lv_obj_align(lbl_date, LV_ALIGN_TOP_MID, -40, 185);

    lbl_week = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_week, &lv_font_cn_24, 0);
    lv_obj_set_style_text_color(lbl_week, C_FG, 0);
    lv_label_set_text(lbl_week, "");
    lv_obj_align(lbl_week, LV_ALIGN_TOP_MID, 40, 185);

    /* === 温度 y=215 === */
    lv_obj_t *ti = lv_label_create(scr);
    lv_obj_set_style_text_font(ti, &lv_font_cn_24, 0);
    lv_obj_set_style_text_color(ti, C_ORG, 0);
    lv_label_set_text(ti, "温度");
    lv_obj_align(ti, LV_ALIGN_TOP_MID, -95, 215);

    bar_temp = lv_bar_create(scr);
    lv_obj_set_size(bar_temp, 80, 8);
    lv_obj_align(bar_temp, LV_ALIGN_TOP_MID, 0, 219);
    lv_bar_set_range(bar_temp, -10, 50);
    lv_bar_set_value(bar_temp, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_temp, C_GRY, 0);
    lv_obj_set_style_bg_color(bar_temp, C_ORG, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_temp, 4, 0);
    lv_obj_set_style_radius(bar_temp, 4, LV_PART_INDICATOR);

    lbl_temp = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_temp, &lv_font_cn_24, 0);
    lv_obj_set_style_text_color(lbl_temp, C_FG, 0);
    lv_label_set_text(lbl_temp, "");
    lv_obj_align(lbl_temp, LV_ALIGN_TOP_MID, 75, 215);

    /* === 湿度 y=242 === */
    lv_obj_t *hi = lv_label_create(scr);
    lv_obj_set_style_text_font(hi, &lv_font_cn_24, 0);
    lv_obj_set_style_text_color(hi, C_GRN, 0);
    lv_label_set_text(hi, "湿度");
    lv_obj_align(hi, LV_ALIGN_TOP_MID, -95, 242);

    bar_humi = lv_bar_create(scr);
    lv_obj_set_size(bar_humi, 80, 8);
    lv_obj_align(bar_humi, LV_ALIGN_TOP_MID, 0, 246);
    lv_bar_set_range(bar_humi, 0, 100);
    lv_bar_set_value(bar_humi, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_humi, C_GRY, 0);
    lv_obj_set_style_bg_color(bar_humi, C_GRN, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_humi, 4, 0);
    lv_obj_set_style_radius(bar_humi, 4, LV_PART_INDICATOR);

    lbl_humi = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_humi, &lv_font_cn_24, 0);
    lv_obj_set_style_text_color(lbl_humi, C_FG, 0);
    lv_label_set_text(lbl_humi, "");
    lv_obj_align(lbl_humi, LV_ALIGN_TOP_MID, 75, 242);

    line(268);

    /* === 宿舍信息 y=275 === */
    lbl_dorm = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_dorm, &lv_font_cn_24, 0);
    lv_obj_set_style_text_color(lbl_dorm, C_FG, 0);
    lv_label_set_text(lbl_dorm, "");
    lv_obj_align(lbl_dorm, LV_ALIGN_TOP_MID, -80, 275);

    lbl_water = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_water, &lv_font_cn_24, 0);
    lv_obj_set_style_text_color(lbl_water, C_BLU, 0);
    lv_label_set_text(lbl_water, "");
    lv_obj_align(lbl_water, LV_ALIGN_TOP_MID, 0, 275);

    lbl_elec = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_elec, &lv_font_cn_24, 0);
    lv_obj_set_style_text_color(lbl_elec, C_ORG, 0);
    lv_label_set_text(lbl_elec, "");
    lv_obj_align(lbl_elec, LV_ALIGN_TOP_MID, 80, 275);

    /* 定时器 */
    lv_timer_create(scroll_cb, 5000, NULL);
    lv_timer_create(tick_cb, 1000, NULL);

    lv_scr_load(scr);
    display_lvgl_unlock();

    ESP_LOGI(TAG, "UI done");

    ui_update_weather(CITY_DISPLAY, 42, "晴", 28, 65,
                      "体感温度30℃", "东风3级", "能见度25千米");
    /* 初始占位：未配置/未刷新时显示 X，配置加载后由 main.c 覆盖 */
    ui_update_dormitory("XX-XXX", "XX", "XX");
}

void ui_update_time(void)
{
    time_t now; struct tm t;
    time(&now); localtime_r(&now, &t);
    if (!ui_lock(50)) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
    lv_label_set_text(lbl_hhmm, buf);
    snprintf(buf, sizeof(buf), "%02d", t.tm_sec);
    lv_label_set_text(lbl_sec, buf);
    static const char *wk[] = {"日","一","二","三","四","五","六"};
    snprintf(buf, sizeof(buf), "%d月%d日", t.tm_mon+1, t.tm_mday);
    lv_label_set_text(lbl_date, buf);
    char w[16]; snprintf(w, sizeof(w), "周%s", wk[t.tm_wday]);
    lv_label_set_text(lbl_week, w);
    ui_unlock();
}

void ui_update_weather(const char *city, int air, const char *wt,
                       int temp, int hum, const char *fl,
                       const char *win, const char *vis)
{
    if (!ui_lock(1000)) return;
    lv_label_set_text(lbl_city, city ? city : "");
    if (air >= 0) {
        const char *lv; lv_color_t bg;
        if (air<=50)      { lv="优"; bg=C_GRN; }
        else if (air<=100) { lv="良"; bg=lv_color_make(0x2C,0x3E,0x00); }
        else if (air<=150) { lv="中"; bg=C_ORG; }
        else               { lv="差"; bg=C_RED; }
        lv_label_set_text(lbl_air, lv);
        lv_obj_set_style_bg_color(lbl_air, bg, 0);
    }
    lv_label_set_text(lbl_wea, wt ? wt : "");
    snprintf(scroll_texts[0], 64, "今日%s", wt?wt:"");
    snprintf(scroll_texts[1], 64, "%s", win?win:"");
    snprintf(scroll_texts[2], 64, "%s", fl?fl:"");
    snprintf(scroll_texts[3], 64, "%s", vis?vis:"");
    snprintf(scroll_texts[4], 64, "空气指数%d", air);
    if (temp > -100) {
        char b[16]; snprintf(b, sizeof(b), "%d℃", temp);
        lv_label_set_text(lbl_temp, b);
        lv_bar_set_value(bar_temp, temp, LV_ANIM_OFF);
        lv_color_t tc;
        if (temp>=40) tc=C_RED; else if (temp>=35) tc=C_ORG;
        else if (temp>0) tc=C_GRN; else tc=C_GRY;
        lv_obj_set_style_bg_color(bar_temp, tc, LV_PART_INDICATOR);
    }
    if (hum >= 0) {
        char b[16]; snprintf(b, sizeof(b), "%d%%", hum);
        lv_label_set_text(lbl_humi, b);
        lv_bar_set_value(bar_humi, hum, LV_ANIM_OFF);
    }
    ui_unlock();
}

void ui_update_dormitory(const char *dn, const char *w, const char *e)
{
    if (!ui_lock(1000)) return;
    if (dn) { char b[32]; snprintf(b, sizeof(b), "%s", dn); lv_label_set_text(lbl_dorm, b); }
    if (w)  { char b[32]; snprintf(b, sizeof(b), "水%s", w);  lv_label_set_text(lbl_water, b); }
    if (e)  { char b[32]; snprintf(b, sizeof(b), "电%s", e);  lv_label_set_text(lbl_elec, b); }
    ui_unlock();
}

void ui_update_network_status(bool c) { (void)c; }
void ui_update_air(int a, const char *b, const char *c, int d, int e, int f, int g, int h, int i)
{ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i; }
void ui_update_future(const char *a[], const char *b[], const int *c, const int *d)
{ (void)a;(void)b;(void)c;(void)d; }
bool ui_lock(int ms) { return display_lvgl_lock(ms); }
void ui_unlock(void) { display_lvgl_unlock(); }
