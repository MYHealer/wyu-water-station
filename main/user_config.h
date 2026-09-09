/*
 * user_config.h - 用户配置文件
 *
 * 优先使用本地配置文件 user_config.local.h（含真实 WiFi/账号信息，已 gitignore）。
 * 若不存在，则使用本文件中的占位符。复制 user_config.h.example 为
 * user_config.local.h 并填写真实值即可。
 */

#pragma once

/* 优先加载本地真实配置 */
#if defined(__has_include)
#  if __has_include("user_config.local.h")
#    include "user_config.local.h"
#    define HAVE_USER_CONFIG_LOCAL 1
#  endif
#endif

#ifndef HAVE_USER_CONFIG_LOCAL

/* ==== WiFi 配置（占位，请复制 user_config.local.h 填写真实值） ==== */
#define WIFI_SSID       "your_wifi_ssid"
#define WIFI_PASS       "your_wifi_password"

/* ==== 城市 (用于天气查询，英文拼音) ==== */
#define CITY_NAME       "Jiangmen"

/* ==== 城市显示名 (屏幕上显示的中文) ==== */
#define CITY_DISPLAY    "江门"

/* ==== 宿舍信息 (格式: 楼栋-房号，电费API自动解析) ==== */
#define DORM_NUMBER     "46-416"
#define DORM_WATER      "0.00"     /* 水费(元) - 启动后自动查询 */
#define DORM_ELEC       "0.00"     /* 电费(元) - 启动后自动查询 */

/* ==== 电费 API 配置 (五邑大学校园网) ==== */
#define ELEC_API_URL    "http://202.192.240.231/scp-api/electricity-recharge/getCurrentRemaining_v2"
#define ELEC_USER_TYPE  "1"        /* 1=学生 */

/* ==== 天气刷新间隔 (毫秒) ==== */
#define WEATHER_INTERVAL_MS  (60 * 60 * 1000)  /* 60分钟 */

/* ==== 乐校通水费 API 配置（占位，请复制 user_config.local.h 填写真实值） ==== */
#define WATER_PHONE         "your_phone"       /* 登录手机号 */
#define WATER_PASS          "your_password"    /* 登录密码 */
#define WATER_SIGN_KEY      "1FF75E512"        /* 签名密钥（固定） */
#define WATER_SCHOOL_ID     "80790"            /* 学校 ID（五邑大学） */
#define WATER_INVESTOR_ID   "600000000000000047" /* 投资方 ID */

#endif /* HAVE_USER_CONFIG_LOCAL */
