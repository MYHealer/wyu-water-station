/*
 * display.h - ST7789 + LVGL 显示驱动
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"

esp_err_t display_init(void);

/* LVGL 互斥锁: 跨任务操作 LVGL 对象前必须加锁 */
bool display_lvgl_lock(int timeout_ms);
void display_lvgl_unlock(void);
