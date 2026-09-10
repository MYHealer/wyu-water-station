/*
 * main.c - DuduClock ESP-IDF (ESP32-C3)
 *
 * 启动流程:
 *   1. NVS init
 *   2. display init (ST7789 + LVGL)
 *   3. UI init (天气主页面)
 *   4. WiFi 连接
 *   5. NTP 对时
 *   6. 天气 API 查询
 *   7. 时钟 1s 刷新 + 天气 60min 刷新
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_http_client.h"
#include "esp_spiffs.h"
#include "cJSON.h"

#include "config.h"
#include "user_config.h"
#include "display.h"
#include "ui.h"
#include "water_api.h"
#include "DialerClient.h"
#include "web_config.h"
#include "wifi_manager.h"

static const char *TAG = TAG_APP;

/* esurfing 认证成功标志（串行：认证完成后才查水电费） */
extern bool g_auth_success;

/* ---- NVS 存储 ---- */
static nvs_handle_t nvs;

/* ---- Web 配置（WiFi/校园网/乐校通） ---- */
static app_config_t g_cfg;

/* ---- SPIFFS 挂载 + 自检（损坏则格式化自愈） ---- */
static void init_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "spiffs",
        .max_files = 5,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS 挂载失败: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "SPIFFS 就绪");

    /* 自检：尝试写/删测试文件，失败说明文件系统损坏，格式化重建 */
    bool ok = false;
    FILE *f = fopen("/spiffs/.selfcheck", "w");
    if (f) {
        ok = (fwrite("ok", 1, 2, f) == 2);
        fclose(f);
        remove("/spiffs/.selfcheck");
    }
    if (!ok) {
        /* 备份用户配置文件 → 格式化 → 恢复，避免格式化清空配置 */
        ESP_LOGW(TAG, "SPIFFS 自检失败，备份配置后格式化...");
        char *cfg_json = NULL, *esurf_json = NULL;
        long cfg_len = 0, esurf_len = 0;

        FILE *cf = fopen("/spiffs/config.json", "r");
        if (cf) {
            fseek(cf, 0, SEEK_END); cfg_len = ftell(cf); fseek(cf, 0, SEEK_SET);
            if (cfg_len > 0) {
                cfg_json = malloc(cfg_len + 1);
                if (cfg_json && fread(cfg_json, 1, cfg_len, cf) == (size_t)cfg_len) cfg_json[cfg_len] = '\0';
                else { free(cfg_json); cfg_json = NULL; cfg_len = 0; }
            }
            fclose(cf);
        }
        FILE *ef = fopen("/spiffs/ESurfingClient.json", "r");
        if (ef) {
            fseek(ef, 0, SEEK_END); esurf_len = ftell(ef); fseek(ef, 0, SEEK_SET);
            if (esurf_len > 0) {
                esurf_json = malloc(esurf_len + 1);
                if (esurf_json && fread(esurf_json, 1, esurf_len, ef) == (size_t)esurf_len) esurf_json[esurf_len] = '\0';
                else { free(esurf_json); esurf_json = NULL; esurf_len = 0; }
            }
            fclose(ef);
        }

        esp_vfs_spiffs_unregister("spiffs");
        esp_spiffs_format("spiffs");
        esp_vfs_spiffs_register(&conf);
        ESP_LOGI(TAG, "SPIFFS 已格式化重建");

        if (cfg_json && cfg_len > 0) {
            FILE *w = fopen("/spiffs/config.json", "w");
            if (w) { fwrite(cfg_json, 1, cfg_len, w); fclose(w); }
            free(cfg_json);
            ESP_LOGI(TAG, "config.json 已恢复");
        }
        if (esurf_json && esurf_len > 0) {
            FILE *w = fopen("/spiffs/ESurfingClient.json", "w");
            if (w) { fwrite(esurf_json, 1, esurf_len, w); fclose(w); }
            free(esurf_json);
            ESP_LOGI(TAG, "ESurfingClient.json 已恢复");
        }
    }
}

static void nvs_save_str(const char *key, const char *val)
{
    nvs_set_str(nvs, key, val);
    nvs_commit(nvs);
}

static bool nvs_load_str(const char *key, char *buf, size_t len)
{
    return nvs_get_str(nvs, key, buf, &len) == ESP_OK;
}

static void nvs_clear(void)
{
    nvs_erase_all(nvs);
    nvs_commit(nvs);
}

/* ---- 天气结构 ---- */
typedef struct {
    char city[32];
    char weather_text[32];
    int  weather_icon;
    int  temp;
    int  humidity;
    char feelsLike[32];
    char win[32];
    char vis[32];
    int  air;
    bool valid;
} weather_data_t;

static weather_data_t g_weather = { .valid = false };

/* WiFi 由 wifi_manager 统一管理（AP 常开供 Web 配置 + STA 连校园网） */

/* ---- NTP ---- */
static void ntp_init(void)
{
    ESP_LOGI(TAG, "NTP 对时...");
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "ntp5.ict.ac.cn");
    esp_sntp_init();

    /* 设置时区 UTC+8 */
    setenv("TZ", "CST-8", 1);
    tzset();

    /* 等待对时完成（最多 15 秒） */
    for (int i = 0; i < 15; i++) {
        struct tm t;
        time_t now;
        time(&now);
        localtime_r(&now, &t);
        if (t.tm_year > 120) {
            ESP_LOGI(TAG, "NTP 对时成功: %d-%02d-%02d %02d:%02d:%02d",
                     t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                     t.tm_hour, t.tm_min, t.tm_sec);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGW(TAG, "NTP 对时超时，时间可能不准");
}

/* ---- wttr.in 天气解析（免费，无需 key） ---- */

/* 英文天气描述转中文 (wttr.in weatherDesc 映射) */
static const char *weather_to_cn(const char *en)
{
    if (!en || !en[0]) return "未知";
    /* 先匹配长模式，再匹配短模式 */
    if (strstr(en, "Partly cloudy") || strstr(en, "Partly Cloudy")) return "多云";
    if (strstr(en, "Light rain") || strstr(en, "Patchy rain"))      return "小雨";
    if (strstr(en, "Heavy rain") || strstr(en, "Moderate rain"))    return "中雨";
    if (strstr(en, "Thunderstorm") || strstr(en, "thunder"))        return "雷";
    if (strstr(en, "Blizzard") || strstr(en, "Heavy snow"))         return "暴雪";
    if (strstr(en, "Light snow") || strstr(en, "Patchy snow"))      return "小雪";
    if (strstr(en, "rain") || strstr(en, "Rain"))                   return "雨";
    if (strstr(en, "snow") || strstr(en, "Snow"))                   return "雪";
    if (strstr(en, "Sunny") || strstr(en, "Clear"))                 return "晴";
    if (strstr(en, "Cloudy") || strstr(en, "Overcast"))             return "阴";
    if (strstr(en, "Fog") || strstr(en, "fog"))                     return "雾";
    if (strstr(en, "Mist") || strstr(en, "mist"))                   return "雾";
    if (strstr(en, "Haze") || strstr(en, "haze"))                   return "霾";
    if (strstr(en, "Smoky") || strstr(en, "smoky"))                 return "霾";
    if (strstr(en, "Drizzle") || strstr(en, "drizzle"))             return "小雨";
    return "未知";
}

static bool parse_wttr(const char *json)
{
    /* 只提取 current_condition 数组，避免大 JSON 解析失败 */
    const char *cc_start = strstr(json, "\"current_condition\"");
    if (!cc_start) { ESP_LOGW(TAG, "no current_condition"); return false; }
    const char *arr = strchr(cc_start, '[');
    if (!arr) return false;
    int depth = 0;
    const char *p = arr;
    while (*p) {
        if (*p == '[') depth++;
        if (*p == ']') { depth--; if (depth == 0) break; }
        p++;
    }
    if (*p != ']') return false;
    int arr_len = p - arr + 1;
    char *mini = malloc(arr_len + 40);
    if (!mini) return false;
    snprintf(mini, arr_len + 40, "{\"current_condition\":%.*s}", arr_len, arr);

    cJSON *root = cJSON_Parse(mini);
    free(mini);
    if (!root) { ESP_LOGW(TAG, "cJSON parse failed"); return false; }

    cJSON *cc = cJSON_GetObjectItem(root, "current_condition");
    if (!cc || !cJSON_IsArray(cc) || cJSON_GetArraySize(cc) == 0) {
        cJSON_Delete(root);
        return false;
    }
    cJSON *cur = cJSON_GetArrayItem(cc, 0);

    cJSON *j_temp = cJSON_GetObjectItem(cur, "temp_C");
    cJSON *j_hum  = cJSON_GetObjectItem(cur, "humidity");
    cJSON *j_feel = cJSON_GetObjectItem(cur, "FeelsLikeC");
    cJSON *j_vis  = cJSON_GetObjectItem(cur, "visibility");
    cJSON *j_wspd = cJSON_GetObjectItem(cur, "windspeedKmph");
    cJSON *j_wdir = cJSON_GetObjectItem(cur, "winddir16Point");
    cJSON *j_desc = cJSON_GetObjectItem(cur, "weatherDesc");

    if (!j_temp) { cJSON_Delete(root); return false; }

    const char *desc_en = "";
    if (j_desc && cJSON_IsArray(j_desc) && cJSON_GetArraySize(j_desc) > 0) {
        cJSON *d = cJSON_GetArrayItem(j_desc, 0);
        if (cJSON_IsObject(d)) {
            cJSON *val = cJSON_GetObjectItem(d, "value");
            if (val) desc_en = val->valuestring;
        }
    }

    const char *cn = weather_to_cn(desc_en);
    strncpy(g_weather.weather_text, cn, sizeof(g_weather.weather_text) - 1);
    g_weather.temp = atoi(j_temp->valuestring);
    g_weather.humidity = j_hum ? atoi(j_hum->valuestring) : 0;

    if (j_feel)
        snprintf(g_weather.feelsLike, sizeof(g_weather.feelsLike),
                 "体感温度%s℃", j_feel->valuestring);
    if (j_wdir && j_wspd)
        snprintf(g_weather.win, sizeof(g_weather.win),
                 "%s风%skm/h", j_wdir->valuestring, j_wspd->valuestring);
    if (j_vis)
        snprintf(g_weather.vis, sizeof(g_weather.vis),
                 "能见度%s千米", j_vis->valuestring);

    g_weather.air = 42; /* wttr.in 无 AQI，给默认值 */
    g_weather.valid = true;
    cJSON_Delete(root);
    return true;
}

/* 简易 HTTP GET，返回分配的响应体（调用方 free） */
static char *http_get(const char *url, int timeout_ms)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = timeout_ms,
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return NULL;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http_open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return NULL;
    }
    esp_http_client_fetch_headers(client);
    int total = 0, cap = 8192;
    char *buf = malloc(cap);
    if (!buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return NULL;
    }
    int read;
    while ((read = esp_http_client_read(client, buf + total, cap - total - 1)) > 0) {
        total += read;
        if (total >= cap - 1) {
            /* 扩容继续读，避免截断大响应 */
            int new_cap = cap * 2;
            char *nb = realloc(buf, new_cap);
            if (!nb) break; /* 内存不足，用已读部分 */
            buf = nb;
            cap = new_cap;
        }
    }
    buf[total] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return buf;
}

static void fetch_weather(void)
{
    /* 城市固定为江门（CITY_NAME/CITY_DISPLAY 来自 user_config.h） */
    const char *city = CITY_NAME;
    const char *city_disp = CITY_DISPLAY;
    char url[256];
    ESP_LOGI(TAG, "查询天气 (wttr.in)...");

    snprintf(url, sizeof(url),
             "http://wttr.in/%s?format=j1", city);
    char *body = http_get(url, 10000);
    ESP_LOGI(TAG, "http_get returned: %s", body ? "OK" : "NULL");
    if (body) {
        ESP_LOGI(TAG, "Response len=%d", strlen(body));
        if (parse_wttr(body)) {
            ESP_LOGI(TAG, "天气: %s %d℃ %d%%",
                     g_weather.weather_text, g_weather.temp, g_weather.humidity);
        } else {
            ESP_LOGW(TAG, "parse_wttr failed");
        }
        free(body);
    }

    if (g_weather.valid) {
        strncpy(g_weather.city, city_disp, sizeof(g_weather.city) - 1);
        ui_update_weather(city_disp, g_weather.air,
                          g_weather.weather_text, g_weather.temp,
                          g_weather.humidity, g_weather.feelsLike,
                          g_weather.win, g_weather.vis);
    }
}

/* ---- 水费查询 (乐校通 API) ---- */
static void fetch_water(void)
{
    ESP_LOGI(TAG, "查询水费...");
    float balance = water_query_all();
    if (balance >= 0) {
        char water_str[16];
        snprintf(water_str, sizeof(water_str), "%.1f", balance);
        ESP_LOGI(TAG, "水费余额: %s 元", water_str);
        ui_update_dormitory(NULL, water_str, NULL);
    } else {
        ESP_LOGW(TAG, "水费查询失败 (%.0f)", balance);
    }
}

/* ---- 电费查询 (POST, 五邑大学校园网) ---- */
/* HTTP POST 响应缓冲 */
typedef struct {
    char *data;
    int len;
    int cap;
} http_buf_t;

static esp_err_t http_post_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        http_buf_t *buf = evt->user_data;
        if (buf->data && evt->data_len > 0) {
            int remain = buf->cap - buf->len - 1;
            if (remain > 0) {
                int copy = (evt->data_len < remain) ? evt->data_len : remain;
                memcpy(buf->data + buf->len, evt->data, copy);
                buf->len += copy;
                buf->data[buf->len] = '\0';
            }
        }
    }
    return ESP_OK;
}

static char *http_post(const char *url, const char *post_data,
                       const char *content_type, int timeout_ms)
{
    char *resp_buf = malloc(2048);
    if (!resp_buf) return NULL;
    http_buf_t hbuf = { .data = resp_buf, .len = 0, .cap = 2048 };

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = timeout_ms,
        .skip_cert_common_name_check = true,
        .event_handler = http_post_event_handler,
        .user_data = &hbuf,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(resp_buf); return NULL; }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_header(client, "User-Agent", "Mozilla/5.0");
    esp_http_client_set_header(client, "Referer", "http://202.192.240.231/recharge.html");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "http_post: status=%d, body_len=%d", status, hbuf.len);

    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || hbuf.len == 0) {
        free(resp_buf);
        return NULL;
    }
    return resp_buf;
}

static void fetch_electricity(void)
{
    /* 从 dorm_number 解析楼栋和房号 (格式: "46-416") */
    const char *dorm = g_cfg.dorm_number[0] ? g_cfg.dorm_number : DORM_NUMBER;
    char building[8] = "46", room[8] = "416";
    const char *dash = strchr(dorm, '-');
    if (dash) {
        int blen = dash - dorm;
        if (blen > 0 && blen < (int)sizeof(building)) {
            strncpy(building, dorm, blen);
            building[blen] = '\0';
        }
        strncpy(room, dash + 1, sizeof(room) - 1);
    }

    char post_data[128];
    snprintf(post_data, sizeof(post_data),
             "userTypeID=%s&building=%s&room=%s",
             ELEC_USER_TYPE, building, room);

    ESP_LOGI(TAG, "查询电费: building=%s room=%s", building, room);
    char *body = http_post(ELEC_API_URL, post_data,
                           "application/x-www-form-urlencoded", 20000);
    if (!body) {
        ESP_LOGW(TAG, "电费查询失败");
        return;
    }

    ESP_LOGI(TAG, "电费响应: %s", body);
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return;

    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (data) {
        cJSON *resamp = cJSON_GetObjectItem(data, "resamp");
        if (resamp) {
            char elec_str[16];
            if (cJSON_IsNumber(resamp)) {
                snprintf(elec_str, sizeof(elec_str), "%.1f", resamp->valuedouble);
            } else if (cJSON_IsString(resamp)) {
                strncpy(elec_str, resamp->valuestring, sizeof(elec_str) - 1);
            } else {
                cJSON_Delete(root);
                return;
            }
            ESP_LOGI(TAG, "电费余额: %s", elec_str);
            ui_update_dormitory(NULL, NULL, elec_str);
        }
    }
    cJSON_Delete(root);
}

/* ---- 后台任务 ---- */

/* 时钟刷新 1秒 */
static void clock_task(void *arg)
{
    while (1) {
        ui_update_time();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* 天翼认证 task（work() 是阻塞守护循环，必须放独立 task） */
static TaskHandle_t g_auth_handle = NULL;

static void auth_task(void *arg)
{
    ESP_LOGI(TAG, "校园网认证 task 启动");
    g_auth_handle = xTaskGetCurrentTaskHandle();
    work(); /* 永不返回（内部守护死循环） */
}

/* 天气+水电费刷新 60分钟（启动后：等WiFi→认证完成(串行)→按配置查询） */
static void weather_task(void *arg)
{
    bool need_auth = g_cfg.campus_username[0];

    /* 等 WiFi 就绪（最多 60 秒） */
    if (!wifi_wait_connected(60000)) {
        ESP_LOGW(TAG, "STA 未连接，仅 AP 可用");
    } else if (need_auth) {
        ESP_LOGI(TAG, "STA 已连接，启动校园网认证（后台）...");
        xTaskCreate(auth_task, "auth", 8192, NULL, 1, NULL);
    } else {
        ESP_LOGI(TAG, "未配置校园网账号，跳过认证");
    }

    /* 串行：如果配置了认证，等认证成功（最多 30s）再查水电费，
       避免认证与水费查询并发占用堆导致验证码解码 OOM */
    if (need_auth) {
        ESP_LOGI(TAG, "等待校园网认证完成...");
        for (int i = 0; i < 30 && !g_auth_success; i++)
            vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "认证%s，开始查询", g_auth_success ? "成功" : "超时/未成功");
    }

    vTaskDelay(pdMS_TO_TICKS(5000));
    /* 显示配置的宿舍号（覆盖初始 XX-XXX 占位） */
    if (g_cfg.dorm_number[0]) ui_update_dormitory(g_cfg.dorm_number, NULL, NULL);
    /* 查询期间挂起认证 task，避免其 keep-alive HTTP 制造堆碎片
       导致验证码解码的大块开销分配失败 */
    if (g_auth_handle) vTaskSuspend(g_auth_handle);
    fetch_weather(); /* 城市固定江门 */
    if (g_cfg.dorm_number[0]) fetch_electricity();
    if (g_cfg.water_phone[0]) fetch_water();
    if (g_auth_handle) vTaskResume(g_auth_handle);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(WEATHER_INTERVAL_MS));
        if (wifi_is_connected()) {
            if (g_auth_handle) vTaskSuspend(g_auth_handle);
            fetch_weather();
            if (g_cfg.dorm_number[0]) fetch_electricity();
            if (g_cfg.water_phone[0]) fetch_water();
            if (g_auth_handle) vTaskResume(g_auth_handle);
        }
    }
}

/* ---- 入口 ---- */
void app_main(void)
{
    ESP_LOGI(TAG, "=== DuduClock ESP-IDF 启动 ===");

    /* 打印上次重启原因（诊断随机重启用） */
    {
        esp_reset_reason_t reason = esp_reset_reason();
        const char *reason_str[] = {
            [ESP_RST_UNKNOWN]   = "UNKNOWN",
            [ESP_RST_POWERON]   = "POWERON",
            [ESP_RST_EXT]       = "EXT_PIN",
            [ESP_RST_SW]        = "SW",
            [ESP_RST_PANIC]     = "PANIC",
            [ESP_RST_INT_WDT]   = "INT_WDT",
            [ESP_RST_TASK_WDT]  = "TASK_WDT",
            [ESP_RST_WDT]       = "WDT",
            [ESP_RST_DEEPSLEEP] = "DEEPSLEEP",
            [ESP_RST_BROWNOUT]  = "BROWNOUT",
            [ESP_RST_SDIO]      = "SDIO",
        };
        const char *rstr = (reason >= 0 && reason <= ESP_RST_SDIO) ? reason_str[reason] : "?";
        ESP_LOGI(TAG, "上次重启原因: %s (%d)", rstr, reason);
        if (reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
            reason == ESP_RST_TASK_WDT) {
            ESP_LOGE(TAG, "!!! 检测到异常重启，请检查串口日志 !!!");
        }
    }

    /* 降低 ESURF 日志级别，减少 UART mutex 并发冲突导致的 FreeRTOS assert */
    esp_log_level_set("ESURF", ESP_LOG_WARN);
    esp_log_level_set("DialerClient", ESP_LOG_WARN);
    esp_log_level_set("NetClient", ESP_LOG_WARN);

    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    nvs_open("clock", NVS_READWRITE, &nvs);

    /* 显示 */
    ESP_ERROR_CHECK(display_init());

    /* UI */
    ui_weather_page_init();
    ui_update_time();

    /* SPIFFS */
    init_spiffs();

    /* WiFi 初始化（建 netif + AP 常开；连接在配置加载后进行） */
    if (wifi_init() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 初始化失败");
    }

    /* Web 配置后台（netif 就绪后启动 HTTP server） */
    web_config_start();

    /* 读取配置（WiFi/校园网/乐校通） */
    memset(&g_cfg, 0, sizeof(g_cfg));
    if (load_config(&g_cfg)) {
        ESP_LOGI(TAG, "已加载配置: WiFi=%s 校园网=%s 宿舍=%s 城市=%s",
                 g_cfg.wifi_ssid, g_cfg.campus_username,
                 g_cfg.dorm_number[0] ? g_cfg.dorm_number : "-",
                 g_cfg.city[0] ? g_cfg.city : CITY_NAME);
    } else {
        ESP_LOGW(TAG, "无配置，连 AP: ESurfing-Config → http://192.168.4.1 配置");
    }

    /* 配置乐校通账号（水费查询） */
    water_api_set_config(g_cfg.water_phone, g_cfg.water_password,
                         g_cfg.dorm_number);

    /* 连接 WiFi（STA 连校园网） */
    if (g_cfg.wifi_ssid[0]) {
        ESP_LOGI(TAG, "连接 WiFi: %s", g_cfg.wifi_ssid);
        wifi_connect(g_cfg.wifi_ssid, g_cfg.wifi_password);
    }

    /* NTP */
    ntp_init();

    /* 后台任务 */
    xTaskCreate(clock_task, "clock", 4096, NULL, 3, NULL);
    xTaskCreate(weather_task, "weather", 24576, NULL, 2, NULL);

    ESP_LOGI(TAG, "主任务退出，由后台任务接管");
}
