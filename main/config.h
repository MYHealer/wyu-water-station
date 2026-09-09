/*
 * config.h - DuduClock 硬件引脚与屏幕常量
 *
 * 引脚来源: E:\Downloads\User_Setup.h (TFT_eSPI 配置)
 * 芯片: ESP32-C3
 */

#pragma once

#include <stdint.h>
#include "driver/spi_master.h"

/* ===== SPI 主机 =====
 * ESP32-C3 只有一个通用 SPI (SPI2_HOST)，FSPI 即 SPI2 */
#define LCD_HOST        SPI2_HOST

/* ===== ST7789 引脚 (照抄 User_Setup.h) ===== */
#define PIN_LCD_MOSI    3
#define PIN_LCD_SCLK    2
#define PIN_LCD_CS      7
#define PIN_LCD_DC      4
#define PIN_LCD_RST     5
/* 无 MISO (User_Setup.h 中 TFT_MISO 被注释) */
/* 无背光控制引脚，背光硬件直连 */

#define PIN_LCD_MISO    -1

/* ===== 屏幕参数 ===== */
#define LCD_H_RES       240
#define LCD_V_RES       320
#define LCD_PIXEL_CLK   (26 * 1000 * 1000)

/* ===== LVGL ===== */
#define LVGL_TICK_PERIOD_MS     2
#define LVGL_TASK_STACK_SIZE    (6 * 1024)
#define LVGL_TASK_PRIORITY      2
#define LVGL_TASK_MAX_DELAY_MS  500
#define LVGL_TASK_MIN_DELAY_MS  1
/* C3 无 PSRAM，用行缓冲: 240 * 40 * 2B = 19.2KB */
#define LVGL_BUF_HEIGHT         40

/* ===== 按钮 (来自原工程 common.h: #define BUTTON 8) ===== */
#define PIN_BUTTON      8

/* ===== 指示灯 (来自原工程 common.h: #define D4 12) ===== */
#define PIN_LED         12

/* ===== 日志标签 ===== */
#define TAG_DISP    "DISP"
#define TAG_APP     "APP"
