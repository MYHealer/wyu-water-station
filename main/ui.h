/*
 * ui.h - DuduClock LVGL UI 接口
 *
 * 4 个页面：天气主页 / 空气质量 / 未来天气 / 宿舍信息
 * GPIO 8 按键：单击=计时器启停, 双击=切换页面, 长按=计时器重置
 */
#pragma once

#include "lvgl.h"
#include <stdbool.h>

/* 初始化所有页面（调用后，由定时器驱动刷新） */
void ui_weather_page_init(void);

/* 更新时间显示（每秒调用） */
void ui_update_time(void);

/* 更新天气数据 */
void ui_update_weather(const char *city, int air, const char *weather_text,
                       int temp, int humidity, const char *feelsLike,
                       const char *win, const char *vis);

/* 更新空气质量页面 */
void ui_update_air(int aqi, const char *level, const char *color,
                   int pm10, int pm25, int no2, int so2, int co_x10, int o3);

/* 更新未来天气页面 (6日) */
void ui_update_future(const char *dates[], const char *types[],
                      const int *los, const int *his);

/* 更新宿舍信息页面 */
void ui_update_dormitory(const char *dorm_number, const char *water,
                         const char *elec);

/* 更新校园网状态 */
void ui_update_network_status(bool connected);

/* LVGL 互斥锁（跨任务操作 UI 前必须加锁） */
bool ui_lock(int timeout_ms);
void ui_unlock(void);
