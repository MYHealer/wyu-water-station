/*
 * water_api.c - 乐校通水费查询 (ESP32-C3)
 *
 * 流程：登录(CNN验证码) → 设备发现 → 余额查询
 * 依赖：mbedtls (MD5/base64), cJSON, esp_http_client, NVS, miniz
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "cJSON.h"
#include "mbedtls/md5.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* PNG 解压 (zlib deflate, ESP32-C3 ROM 内置) */
#include "miniz.h"

/* CNN 权重 + 推理函数 */
#include "captcha_cnn_weights.h"

#include "water_api.h"
#include "user_config.h"

static const char *TAG = "WATER";

/* ---- 乐校通 API 常量 ---- */
#define LXT_BASE_URL    "https://v3-prod-beta-app.lxt6.cn"
#define LXT_SIGN_KEY    WATER_SIGN_KEY
#define LXT_SCHOOL_ID   WATER_SCHOOL_ID
#define LXT_INVESTOR_ID WATER_INVESTOR_ID
#define LXT_SID         "2021011300001"
#define LXT_VER         "4.3.9"
#define LXT_CTYPE       "1"

/* NVS keys */
#define NVS_TOKEN_KEY    "water_token"
#define NVS_MACHINE_KEY  "water_machid"
#define NVS_DORM_KEY     "water_dorm"

/* ---- 全局状态 ---- */
static char g_token[512] = {0};
static char g_machine_id[128] = {0};
static nvs_handle_t g_water_nvs = 0;
static char g_water_phone[32] = {0};   /* 乐校通登录手机号（Web 配置） */
static char g_water_pass[64] = {0};     /* 乐校通登录密码 */
static char g_dorm[16] = {0};           /* 宿舍号 "46-416" */

void water_api_set_config(const char *phone, const char *pass,
                          const char *dorm_number)
{
    if (phone) strncpy(g_water_phone, phone, sizeof(g_water_phone) - 1);
    if (pass)  strncpy(g_water_pass, pass, sizeof(g_water_pass) - 1);
    if (dorm_number) strncpy(g_dorm, dorm_number, sizeof(g_dorm) - 1);
}

/* ============================================================
 * 工具函数
 * ============================================================ */

/* MD5 → 32 字符小写 hex */
static void md5_hex(const char *input, char output[33])
{
    unsigned char digest[16];
    mbedtls_md5((const unsigned char *)input, strlen(input), digest);
    for (int i = 0; i < 16; i++)
        sprintf(output + i * 2, "%02x", digest[i]);
    output[32] = '\0';
}

/* X-Sign = MD5(SIGN_KEY + params) */
static void calc_sign(const char *params, char output[33])
{
    char buf[512];
    snprintf(buf, sizeof(buf), "%s%s", LXT_SIGN_KEY, params);
    md5_hex(buf, output);
}

/* Base64 编码，返回输出长度 */
static int b64_encode(const uint8_t *src, size_t slen, char *dst, size_t dlen)
{
    size_t olen = 0;
    mbedtls_base64_encode((unsigned char *)dst, dlen, &olen, src, slen);
    return (int)olen;
}

/* X-Ghost: 16 字符小写 hex (checksum + timestamp_hex + xor) */
static void gen_ghost(char out[17])
{
    /* 使用实际时钟时间（毫秒），不是启动时间 */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    /* 时间戳转 hex，补零到 12 字符 */
    char ts[13];
    snprintf(ts, sizeof(ts), "%012llx", (unsigned long long)ms);
    ts[12] = '\0';

    /* 对 hex 字符串按字节对解析: 每 2 字符解释为 0-255 字节值 累加/异或 */
    int total = 0;
    uint8_t xr = 0;
    for (int i = 0; i < 12; i += 2) {
        uint8_t byte = 0;
        for (int j = 0; j < 2; j++) {
            char c = ts[i + j];
            int v;
            if (c >= '0' && c <= '9')      v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else                           v = c - 'A' + 10;
            byte = (byte << 4) | (uint8_t)v;
        }
        total += byte;
        xr ^= byte;
    }
    /* 输出: 2(hex checksum) + 12(hex ts) + 2(hex xor) = 16 字符 */
    snprintf(out, 17, "%02x%s%02x", total % 256, ts, xr);
    ESP_LOGI(TAG, "gen_ghost: ms=%lld ts=%s out=%s", (long long)ms, ts, out);
}

/* 通用请求头 */
static void set_common_headers(esp_http_client_handle_t client)
{
    esp_http_client_set_header(client, "X-Sid", LXT_SID);
    esp_http_client_set_header(client, "X-Product-Ver", LXT_VER);
    esp_http_client_set_header(client, "X-clientType", LXT_CTYPE);
    char ghost[17];
    gen_ghost(ghost);
    esp_http_client_set_header(client, "X-Ghost", ghost);
    esp_http_client_set_header(client, "User-Agent", "okhttp/3.12.1");
    if (g_token[0]) {
        esp_http_client_set_header(client, "Cookie", g_token);
    }
}

/* ============================================================
 * HTTP 请求封装（event handler 捕获响应体）
 * ============================================================ */

typedef struct {
    char *data;
    int len;
    int cap;
    int status;
    /* 响应头: TokenInfo */
    char token[256];
    /* 流式搜索模式：搜索 name 包含 search_key 的节点，提取节点对象到 search_hit，id 到 search_id */
    int  search_mode;
    char search_key[32];
    char *search_hit;   /* 命中节点对象 JSON（堆分配，由请求方释放） */
    int  search_hit_cap;
    char search_id[64];
    int  search_done;
} lxt_http_buf_t;

/* 在已累计的 JSON 文本里搜索 "name":"<key>" 节点，命中则把完整节点对象 {…} 复制到 search_hit，
 * 并把其 id 提取到 search_id。返回 1 命中，0 未命中 */
static int buf_try_search(lxt_http_buf_t *b)
{
    if (!b->search_mode || b->search_done) return 0;
    if (!b->data || b->len < 4) return 0;

    /* 构造 "name":"<key>" 模式（key 为子串，匹配 "46" 也能匹配 "46栋"） */
    char pat[48];
    snprintf(pat, sizeof(pat), "\"name\":\"%s", b->search_key);

    char *hit = strstr(b->data, pat);
    if (!hit) return 0;

    /* 从命中点回找最近的对象 "{"（括号感知：跳过字符串和嵌套块） */
    const char *obj = hit;
    int bdepth = 0; /* 嵌套深度：遇到 ] 或 } 递增，遇到 [ 或 { 递减 */
    for (int i = 0; i < 2048 && obj > b->data; i++) {
        obj--;
        if (*obj == '"') {
            /* 字符串内回找：跳过整个 "..." 段（含转义） */
            const char *q = obj - 1;
            while (q > b->data && *q != '"') {
                if (*q == '\\') q--; /* 跳过转义字符 */
                q--;
            }
            obj = q; /* 跳到开头引号，继续向前回找 */
            continue;
        }
        if (*obj == ']' || *obj == '}') {
            bdepth++; /* 进入嵌套块 */
            continue;
        }
        if (*obj == '[' || *obj == '{') {
            if (bdepth > 0) {
                bdepth--; /* 跳过嵌套块的开头 */
                continue;
            }
            /* bdepth == 0: 找到了目标对象的 { */
            break;
        }
    }
    if (*obj != '{') return 0;

    /* 括号平衡找匹配的 '}' */
    int depth = 0;
    const char *scan = obj;
    for (; scan < b->data + b->len; scan++) {
        if (*scan == '{') depth++;
        else if (*scan == '}') {
            depth--;
            if (depth == 0) break;
        }
    }
    if (depth != 0 || scan >= b->data + b->len) return 0;
    int obj_len = scan - obj + 1;
    if (obj_len >= b->search_hit_cap) return 0;

    memcpy(b->search_hit, obj, obj_len);
    b->search_hit[obj_len] = '\0';

    /* 从对象里提取 id */
    const char *idk = strstr(obj, "\"id\":\"");
    if (idk) {
        idk += 6;
        const char *e = idk;
        while (*e && *e != '"') e++;
        int vlen = e - idk;
        if (vlen > 0 && vlen < (int)sizeof(b->search_id)) {
            memcpy(b->search_id, idk, vlen);
            b->search_id[vlen] = '\0';
        }
    }

    b->search_done = 1;
    return 1;
}

static esp_err_t lxt_http_event(esp_http_client_event_t *evt)
{
    lxt_http_buf_t *b = evt->user_data;
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (b->data && evt->data_len > 0) {
            if (b->search_done) break; /* 已命中，丢弃后续 */
            /* 动态扩容（搜索模式最多 48KB，普通模式最多 96KB）。
             * 超出上限则丢弃本块数据，绝不越界写堆。 */
            int cap_limit = b->search_mode ? 49152 : 98304;
            int need = b->len + evt->data_len + 1;
            if (need > b->cap) {
                int new_cap = b->cap * 2;
                while (new_cap < need) new_cap *= 2;
                if (new_cap > cap_limit) new_cap = cap_limit;
                if (new_cap > b->cap) {
                    char *nd = realloc(b->data, new_cap);
                    if (nd) { b->data = nd; b->cap = new_cap; }
                    else return ESP_ERR_NO_MEM;
                }
                if (need > b->cap) {
                    ESP_LOGW(TAG, "ON_DATA 丢弃 %d 字节 (cap=%d)", evt->data_len, b->cap);
                    break; /* 无法容纳，丢弃，不越界 */
                }
            }
            memcpy(b->data + b->len, evt->data, evt->data_len);
            b->len += evt->data_len;
            b->data[b->len] = '\0';
            buf_try_search(b);
        }
        break;
    case HTTP_EVENT_ON_HEADER:
        if (strcasecmp(evt->header_key, "TokenInfo") == 0 ||
            strcasecmp(evt->header_key, "tokenInfo") == 0) {
            snprintf(b->token, sizeof(b->token), "tokenInfo=%s", evt->header_value);
        } else if (strcasecmp(evt->header_key, "set-cookie") == 0) {
            /* Set-Cookie: tokenInfo=<val>; Path=/... */
            const char *p = strcasestr(evt->header_value, "tokenInfo=");
            if (p) {
                strncpy(b->token, p, sizeof(b->token) - 1);
                b->token[sizeof(b->token) - 1] = '\0';
                /* 截断到分号 */
                char *semi = strchr(b->token, ';');
                if (semi) *semi = '\0';
            }
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

/* GET 请求，返回响应体（调用方 free），status 输出状态码 */
static char *lxt_get(const char *path, const char *sign_input, lxt_http_buf_t *buf)
{
    char url[512];
    snprintf(url, sizeof(url), "%s%s", LXT_BASE_URL, path);

    char sign[33];
    calc_sign(sign_input, sign);

    int save_search_mode = buf->search_mode;
    char save_search_key[32];
    memcpy(save_search_key, buf->search_key, sizeof(save_search_key));
    char *save_search_hit = buf->search_hit;
    int save_search_hit_cap = buf->search_hit_cap;

    memset(buf, 0, sizeof(*buf));
    buf->search_mode = save_search_mode;
    memcpy(buf->search_key, save_search_key, sizeof(buf->search_key));
    buf->search_hit = save_search_hit;
    buf->search_hit_cap = save_search_hit_cap;
    buf->data = malloc(4096);
    buf->cap = 4096;
    if (!buf->data) return NULL;

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 25000,
        .skip_cert_common_name_check = true,
        .event_handler = lxt_http_event,
        .user_data = buf,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(buf->data); buf->data = NULL; return NULL; }

    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_header(client, "Accept-Encoding", "identity"); /* 禁用 gzip */
    set_common_headers(client);
    esp_http_client_set_header(client, "X-Sign", sign);

    ESP_LOGI(TAG, "GET %s sign=%s cookie=%s", path, sign,
             g_token[0] ? g_token : "(none)");
    esp_err_t err = esp_http_client_perform(client);
    buf->status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "GET %s status=%d len=%d search=%d hit=%s",
             path, buf->status, buf->len, buf->search_done,
             (buf->search_done && buf->search_id[0]) ? buf->search_id : "-");

    if (err != ESP_OK || buf->status != 200) {
        ESP_LOGW(TAG, "GET %s failed: err=%s status=%d sign=%s", path, esp_err_to_name(err), buf->status, sign);
        free(buf->data);
        buf->data = NULL;
    }
    return buf->data;
}

/* 搜索模式 GET：请求 getLowerAreas 并在流式响应中找 name 含 keyword 的节点。
 * 命中返回 malloc 的对象 JSON（调用方 free），未命中返回 NULL。
 * 只缓冲到命中即止，避免 65KB 整树 OOM。 */
static char *lxt_get_search_node(const char *area_id, const char *keyword)
{
    char path[256], sign_input[128];
    snprintf(path, sizeof(path),
             "/baseDict/site/getLowerAreas?areaId=%s", area_id);
    snprintf(sign_input, sizeof(sign_input),
             "areaId=%s", area_id);

    /* 最多重试 3 次 */
    for (int retry = 0; retry < 3; retry++) {
        /* 先用普通 GET 获取完整响应（不做流式搜索） */
        lxt_http_buf_t buf;
        memset(&buf, 0, sizeof(buf));
        buf.data = malloc(4096);
        buf.cap = 4096;
        if (!buf.data) return NULL;

        char url[512];
        snprintf(url, sizeof(url), "%s%s", LXT_BASE_URL, path);
        char sign[33];
        calc_sign(sign_input, sign);

        esp_http_client_config_t cfg = {
            .url = url,
            .timeout_ms = 25000,
            .skip_cert_common_name_check = true,
            .event_handler = lxt_http_event,
            .user_data = &buf,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) { free(buf.data); continue; }

        esp_http_client_set_method(client, HTTP_METHOD_GET);
        esp_http_client_set_header(client, "Accept-Encoding", "identity");
        set_common_headers(client);
        esp_http_client_set_header(client, "X-Sign", sign);

        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        int64_t content_len = esp_http_client_get_content_length(client);
        esp_http_client_cleanup(client);

        ESP_LOGI(TAG, "search_node[%d] status=%d len=%d content_len=%lld",
                 retry, status, buf.len, (long long)content_len);

        if (err != ESP_OK || status != 200 || !buf.data) {
            ESP_LOGW(TAG, "search_node[%d] GET failed: err=%s", retry, esp_err_to_name(err));
            free(buf.data);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* 在累积的完整响应中搜索 "name":"<keyword>" */
        char pat[48];
        snprintf(pat, sizeof(pat), "\"name\":\"%s", keyword);
        char *hit = strstr(buf.data, pat);
        if (!hit) {
            ESP_LOGW(TAG, "search_node[%d] 未命中 '%s' (len=%d)", retry, keyword, buf.len);
            free(buf.data);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* 括号感知回溯找对象 { */
        const char *obj = hit;
        int bdepth = 0;
        for (int i = 0; i < 2048 && obj > buf.data; i++) {
            obj--;
            if (*obj == '"') {
                const char *q = obj - 1;
                while (q > buf.data && *q != '"') { if (*q == '\\') q--; q--; }
                obj = q;
                continue;
            }
            if (*obj == ']' || *obj == '}') { bdepth++; continue; }
            if (*obj == '[' || *obj == '{') {
                if (bdepth > 0) { bdepth--; continue; }
                break;
            }
        }
        if (*obj != '{') {
            ESP_LOGW(TAG, "search_node[%d] 回溯未找到 '{'", retry);
            free(buf.data);
            continue;
        }

        /* 括号平衡找匹配的 } */
        int depth = 0;
        const char *scan = obj;
        for (; scan < buf.data + buf.len; scan++) {
            if (*scan == '{') depth++;
            else if (*scan == '}') { depth--; if (depth == 0) break; }
        }
        if (depth != 0) {
            ESP_LOGW(TAG, "search_node[%d] 括号不平衡", retry);
            free(buf.data);
            continue;
        }

        int obj_len = scan - obj + 1;
        char *result = malloc(obj_len + 1);
        if (!result) { free(buf.data); return NULL; }
        memcpy(result, obj, obj_len);
        result[obj_len] = '\0';
        free(buf.data);

        ESP_LOGI(TAG, "search_node[%d] 命中: %.160s", retry, result);
        return result; /* 调用方 free */
    }
    return NULL;
}

/* 搜索节点并返回其 id（malloc，调用方 free） */
static char *lxt_get_search(const char *area_id, const char *keyword)
{
    char *node = lxt_get_search_node(area_id, keyword);
    if (!node) return NULL;
    /* 从对象里提取 id（快速 strstr） */
    char *idk = strstr(node, "\"id\":\"");
    char *id = NULL;
    if (idk) {
        idk += 6;
        char *e = idk;
        while (*e && *e != '"') e++;
        id = malloc(e - idk + 1);
        if (id) { memcpy(id, idk, e - idk); id[e - idk] = '\0'; }
    }
    free(node);
    return id;
}

/* POST JSON 请求 */
static char *lxt_post_json(const char *path, const char *json_body, lxt_http_buf_t *buf)
{
    char url[512];
    snprintf(url, sizeof(url), "%s%s", LXT_BASE_URL, path);

    char sign[33];
    calc_sign(json_body, sign);

    memset(buf, 0, sizeof(*buf));
    buf->data = malloc(4096);
    buf->cap = 4096;
    if (!buf->data) return NULL;

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 25000,
        .skip_cert_common_name_check = true,
        .event_handler = lxt_http_event,
        .user_data = buf,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(buf->data); buf->data = NULL; return NULL; }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_post_field(client, json_body, strlen(json_body));
    esp_http_client_set_header(client, "Content-Type", "application/json");
    set_common_headers(client);
    esp_http_client_set_header(client, "X-Sign", sign);

    esp_err_t err = esp_http_client_perform(client);
    buf->status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || buf->status != 200) {
        ESP_LOGW(TAG, "POST %s failed: err=%s status=%d", path, esp_err_to_name(err), buf->status);
        free(buf->data);
        buf->data = NULL;
    }
    return buf->data;
}

/* ============================================================
 * 验证码预处理 + CNN 推理
 * ============================================================ */

/* 从 base64 data URI 解码图片 → RGB 像素数组
 * 返回 malloc 的 RGB 缓冲区（3 bytes/pixel），调用方 free
 * 输出 width/height
 */
static uint8_t *decode_captcha_image(const char *b64_data, int *out_w, int *out_h, int *out_bpp)
{
    /* base64 解码 */
    size_t b64_len = strlen(b64_data);
    size_t img_len = b64_len * 3 / 4 + 4;
    uint8_t *img_data = malloc(img_len);
    if (!img_data) return NULL;

    if (mbedtls_base64_decode(img_data, img_len, &img_len,
                              (const uint8_t *)b64_data, b64_len) != 0) {
        free(img_data);
        return NULL;
    }

    /* 检测格式：PNG magic = 0x89 0x50 0x4E 0x47 */
    int w = 0, h = 0;
    uint8_t *rgb = NULL;

    if (img_len >= 4 && img_data[0] == 0x89 && img_data[1] == 0x50) {
        /* PNG: 从 IHDR 提取宽高 */
        if (img_len >= 24) {
            w = (img_data[16] << 24) | (img_data[17] << 16) | (img_data[18] << 8) | img_data[19];
            h = (img_data[20] << 24) | (img_data[21] << 16) | (img_data[22] << 8) | img_data[23];
        }
        int bit_depth = (img_len > 24) ? img_data[24] : 0;
        int color_type = (img_len > 25) ? img_data[25] : 0;
        ESP_LOGI(TAG, "PNG %dx%d, %d bytes, bit_depth=%d color_type=%d",
                 w, h, (int)img_len, bit_depth, color_type);

        /* color_type: 2=RGB(3), 6=RGBA(4), 0=灰度(1) */
        *out_bpp = (color_type == 6) ? 4 : (color_type == 2) ? 3 : (color_type == 0) ? 1 : 3;

        /* 简易 PNG 解码: 使用 inflate 解压 IDAT */
        /* PNG 结构: signature(8) + IHDR(length+type+data+crc) + IDAT... + IEND */
        /* 对于简单验证码 PNG，用 zlib inflate 解压像素数据 */

        /* 简化方案：用 LVGL 的图片解码器 */
        /* 注意：这个方案需要 LVGL 锁，且在非 UI 线程中使用需谨慎 */
        /* 暂时使用简化的 raw pixel 解析（假设非交错、无 filter） */

        /* 实际方案：直接用 miniz (zlib) inflate */
        /* ESP-IDF 内置 miniz 在 components/esp_rom 或 components/zlib */
        /* 这里用一个更简单的方法：遍历 PNG chunks */

        /* 最终方案：用 cJSON 的方式——直接分配全图 RGB，用 zlib 解压 IDAT */
        /* ESP-IDF 的 zlib 在 components/zlib，但可能未启用 */
        /* 用 mbedtls 的 zlib 也不确定 */

        /* 实际上，ESP-IDF 的 esp_http_client 已经包含了 zlib 支持（用于 gzip） */
        /* 但直接调用 zlib 解压 PNG 需要 filter 处理 */

        /* 最简方案：假设验证码 PNG 是简单的非压缩或 deflate 格式 */
        /* 跳过 PNG 解析，直接用 HTTP 下载原始图片再处理 */

        /* 找到所有 IDAT chunk 并合并到一个缓冲区 */
        int total_idat = 0;
        for (int p = 8; p + 8 < (int)img_len; ) {
            int clen = (img_data[p] << 24) | (img_data[p+1] << 16) |
                       (img_data[p+2] << 8) | img_data[p+3];
            if (img_data[p+4]=='I' && img_data[p+5]=='D' &&
                img_data[p+6]=='A' && img_data[p+7]=='T') {
                total_idat += clen;
            } else if (!total_idat && img_data[p+4]=='I' && img_data[p+5]=='E'
                       && img_data[p+6]=='N' && img_data[p+7]=='D') {
                break;
            }
            p += 12 + clen; /* len(4)+type(4)+data+crc(4) */
        }
        if (total_idat == 0) {
            ESP_LOGE(TAG, "No IDAT chunk found");
            free(img_data);
            return NULL;
        }
        ESP_LOGI(TAG, "total_idat=%d bytes", total_idat);
        uint8_t *idat_merged = malloc(total_idat);
        if (!idat_merged) { free(img_data); return NULL; }
        int idat_fill = 0;
        for (int p = 8; p + 8 < (int)img_len; ) {
            int clen = (img_data[p] << 24) | (img_data[p+1] << 16) |
                       (img_data[p+2] << 8) | img_data[p+3];
            if (img_data[p+4]=='I' && img_data[p+5]=='D' &&
                img_data[p+6]=='A' && img_data[p+7]=='T') {
                memcpy(idat_merged + idat_fill, img_data + p + 8, clen);
                idat_fill += clen;
            }
            p += 12 + clen;
        }

        /* PNG pixels: 每行 = 1 filter 字节 + 像素数据 */
        /* color_type: 2=RGB(3), 6=RGBA(4), 3=调色板(1) */
        int bpp = (color_type == 6) ? 4 : (color_type == 2) ? 3 : (color_type == 0) ? 1 : 3;
        int row_bytes = 1 + w * bpp;
        int raw_len = row_bytes * h;
        uint8_t *raw = malloc(raw_len);
        if (!raw) { free(idat_merged); free(img_data); return NULL; }
        memset(raw, 0, raw_len);

        /* zlib decompress IDAT → raw（一次性解压到内存，返回实解长度） */
        free(img_data); /* IDAT 已合并，原始 PNG 数据不再需要 */
        img_data = NULL;
        size_t out_len = tinfl_decompress_mem_to_mem(
                raw, raw_len, idat_merged, total_idat,
                TINFL_FLAG_PARSE_ZLIB_HEADER);
        free(idat_merged);
        if (out_len == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
            ESP_LOGE(TAG, "inflate failed (raw_len=%d)", raw_len);
            free(raw);
            return NULL;
        }
        ESP_LOGI(TAG, "inflate: out=%u raw_len=%d", (unsigned)out_len, raw_len);

        /* 处理 PNG filter（原地，不额外分配 rgb 缓冲区）。
         * 逐行处理：读 filter → 重建像素 → 去掉 filter 字节左移 → 更新偏移。
         * 峰值内存从 raw+rgb 降为仅 raw，省 ~18KB。 */
        int cur_off = 0;
        for (int y = 0; y < h; y++) {
            uint8_t *row = raw + cur_off;
            uint8_t filter = row[0];
            uint8_t *pixels = row + 1;

            for (int x = 0; x < w * bpp; x++) {
                int val = pixels[x];
                int a = (x >= bpp) ? pixels[x - bpp] : 0;
                int b_val = (y > 0) ? (raw + (y - 1) * w * bpp)[x] : 0;
                int c = (y > 0 && x >= bpp) ? (raw + (y - 1) * w * bpp)[x - bpp] : 0;

                if (filter == 1) val += a;
                else if (filter == 2) val += b_val;
                else if (filter == 3) val += (a + b_val) / 2;
                else if (filter == 4) {
                    int p = a + b_val - c;
                    int pa = abs(p - a), pb = abs(p - b_val), pc = abs(p - c);
                    val += (pa <= pb && pa <= pc) ? a : (pb <= pc) ? b_val : c;
                }
                pixels[x] = val & 0xFF;
            }
            /* filter 处理完，去掉 filter 字节：左移像素数据 */
            memmove(raw + y * w * bpp, pixels, w * bpp);
            cur_off += row_bytes;
        }
        /* 缩小到实际像素大小 */
        rgb = realloc(raw, w * h * bpp);
        if (!rgb) rgb = raw; /* realloc 失败也能用 */
    } else {
        /* JPEG: TODO - 用 TJpgDec (ROM) 解码 */
        ESP_LOGE(TAG, "JPEG not supported yet");
        free(img_data);
        return NULL;
    }

    free(img_data);
    *out_w = w;
    *out_h = h;
    return rgb;
}

/* 验证码 CNN 预测: 输入 RGB 像素 → 输出 5 位数字字符串 */
static bool predict_captcha(const uint8_t *rgb, int w, int h, int bpp, char result[6])
{
    /* 1. 去蓝色干扰线 */
    uint8_t *clean = malloc(w * h * 3);
    if (!clean) return false;
    memcpy(clean, rgb, (size_t)w * h * bpp);

    for (int i = 0; i < w * h; i++) {
        uint8_t r = clean[i * bpp], g = clean[i * bpp + 1], b = clean[i * bpp + 2];
        if (b > r + 50 && b > g + 50) {
            clean[i * bpp] = clean[i * bpp + 1] = clean[i * bpp + 2] = 255;
        }
    }

    /* 2. 灰度化 */
    uint8_t *gray = malloc(w * h);
    if (!gray) { free(clean); return false; }
    for (int i = 0; i < w * h; i++) {
        gray[i] = (uint8_t)(clean[i*bpp] * 0.299f + clean[i*bpp+1] * 0.587f + clean[i*bpp+2] * 0.114f);
    }
    free(clean);

    /* 3. 二值化 */
    uint8_t *bin = malloc(w * h);
    if (!bin) { free(gray); return false; }
    for (int i = 0; i < w * h; i++) {
        bin[i] = (gray[i] < 128) ? 1 : 0;
    }
    free(gray);

    /* 4. 去噪：小连通区域清除 */
    /* 跳过（验证码通常干净） */

    /* 5. 列投影 */
    int *col_sum = calloc(w, sizeof(int));
    if (!col_sum) { free(bin); return false; }
    for (int x = 0; x < w; x++)
        for (int y = 0; y < h; y++)
            col_sum[x] += bin[y * w + x];

    /* 6. 找连续字符区域 */
    typedef struct { int x0, x1; } region_t;
    region_t regions[10];
    int nreg = 0;
    int in_region = 0, start = 0;

    for (int x = 0; x < w && nreg < 10; x++) {
        if (col_sum[x] >= 2) {
            if (!in_region) { start = x; in_region = 1; }
        } else {
            if (in_region) {
                /* 合并间隔 < 3px 的区域 */
                if (nreg > 0 && start - regions[nreg-1].x1 < 3) {
                    regions[nreg-1].x1 = x - 1;
                } else {
                    regions[nreg].x0 = start;
                    regions[nreg].x1 = x - 1;
                    nreg++;
                }
                in_region = 0;
            }
        }
    }
    if (in_region && nreg < 10) {
        if (nreg > 0 && start - regions[nreg-1].x1 < 3) {
            regions[nreg-1].x1 = w - 1;
        } else {
            regions[nreg].x0 = start;
            regions[nreg].x1 = w - 1;
            nreg++;
        }
    }
    free(col_sum);

    if (nreg != 5) {
        ESP_LOGW(TAG, "Expected 5 chars, found %d (w=%d h=%d)", nreg, w, h);
        for (int i = 0; i < nreg; i++)
            ESP_LOGW(TAG, "  region[%d]: x0=%d x1=%d w=%d",
                     i, regions[i].x0, regions[i].x1,
                     regions[i].x1 - regions[i].x0 + 1);
        free(bin);
        return false;
    }

    /* 7. 每个字符: 提取 → 缩放到 12×8 → 二值化 → CNN */
    for (int i = 0; i < 5; i++) {
        int rx0 = regions[i].x0, rx1 = regions[i].x1;
        int rw = rx1 - rx0 + 1;

        /* 找字符的垂直范围 */
        int ry0 = h, ry1 = 0;
        for (int y = 0; y < h; y++)
            for (int x = rx0; x <= rx1; x++)
                if (bin[y * w + x]) {
                    if (y < ry0) ry0 = y;
                    if (y > ry1) ry1 = y;
                }
        if (ry0 > ry1) { ry0 = 0; ry1 = h - 1; }
        /* 加 1px margin */
        if (ry0 > 0) ry0--;
        if (ry1 < h - 1) ry1++;
        int rh = ry1 - ry0 + 1;

        /* 缩放到 12×8 (最近邻)。模型训练输入极性为“背景=1,笔画=0”
         * (Python: (1-char_img)*255 → >128 阈值)，
         * 故写入 (1 - bin) 以匹配训练分布。 */
        uint8_t char_img[12 * 8];
        for (int dy = 0; dy < 12; dy++) {
            int sy = ry0 + dy * rh / 12;
            if (sy >= h) sy = h - 1;
            for (int dx = 0; dx < 8; dx++) {
                int sx = rx0 + dx * rw / 8;
                if (sx > rx1) sx = rx1;
                char_img[dy * 8 + dx] = 1 - bin[sy * w + sx];
            }
        }

        /* CNN 推理 */
        result[i] = '0' + captcha_predict(char_img);
    }
    result[5] = '\0';
    free(bin);
    return true;
}

/* ============================================================
 * 登录流程
 * ============================================================ */

static bool water_login(void)
{
    ESP_LOGI(TAG, "水费登录中...");
    char path[256], sign_input[256];

    /* 尝试登录（最多 10 次，验证码识别可能失败） */
    for (int attempt = 0; attempt < 10; attempt++) {
        /* Step 1: 获取验证码 */
        snprintf(path, sizeof(path),
                 "/user/authentication/getCode?account=%s", g_water_phone);
        snprintf(sign_input, sizeof(sign_input),
                 "account=%s", g_water_phone);

        lxt_http_buf_t buf;
        char *resp = lxt_get(path, sign_input, &buf);
        if (!resp) {
            ESP_LOGW(TAG, "getCode 失败 (status=%d)", buf.status);
            continue;
        }
        ESP_LOGI(TAG, "getCode 响应 len=%d", strlen(resp));

        cJSON *root = cJSON_Parse(resp);
        free(resp);
        if (!root) continue;

        cJSON *code = cJSON_GetObjectItem(root, "Code");
        cJSON *data = cJSON_GetObjectItem(root, "Data");
        if (!cJSON_IsNumber(code) || code->valueint != 0 || !cJSON_IsString(data)) {
            ESP_LOGW(TAG, "getCode 响应异常: Code=%s, Data=%s",
                     code ? (cJSON_IsNumber(code) ? "num" : "not-num") : "null",
                     data ? (cJSON_IsString(data) ? "str" : "not-str") : "null");
            /* 打印响应前200字符 */
            char *dbg = cJSON_PrintUnformatted(root);
            if (dbg) { ESP_LOGW(TAG, "getCode resp: %.200s", dbg); free(dbg); }
            cJSON_Delete(root);
            continue;
        }

        /* 提取 base64 图片数据（逗号后） */
        const char *b64_start = strchr(data->valuestring, ',');
        if (!b64_start) { b64_start = data->valuestring; }
        else { b64_start++; }
        ESP_LOGI(TAG, "验证码图片 base64 长度: %d", strlen(b64_start));

        /* Step 2: 解码图片 + 预处理 + CNN 推理 */
        int img_w = 0, img_h = 0, img_bpp = 3;
        uint8_t *rgb = decode_captcha_image(b64_start, &img_w, &img_h, &img_bpp);
        if (!rgb) {
            ESP_LOGW(TAG, "图片解码失败");
            cJSON_Delete(root);
            continue;
        }

        char captcha[6];
        bool ok = predict_captcha(rgb, img_w, img_h, img_bpp, captcha);
        free(rgb);
        if (!ok) {
            ESP_LOGW(TAG, "验证码识别失败");
            cJSON_Delete(root);
            continue;
        }
        ESP_LOGI(TAG, "验证码识别: %s (attempt %d)", captcha, attempt + 1);
        cJSON_Delete(root);

        /* Step 3: 构造登录请求 */
        /* 内层 JSON */
        char inner[256];
        snprintf(inner, sizeof(inner),
                 "{\"studentMobile\":\"%s\",\"loginPassword\":\"%s\",\"code\":\"%s\"}",
                 g_water_phone, g_water_pass, captcha);

        /* base64 编码内层 */
        size_t b64_len = strlen(inner) * 2;
        char *b64_buf = malloc(b64_len);
        if (!b64_buf) continue;
        b64_encode((const uint8_t *)inner, strlen(inner), b64_buf, b64_len);

        /* 外层 JSON */
        char outer[512];
        snprintf(outer, sizeof(outer), "{\"data\":\"%s\"}", b64_buf);
        free(b64_buf);

        /* POST 登录 */
        lxt_http_buf_t login_buf;
        resp = lxt_post_json("/user/login/userLoginV2WithEncrypt", outer, &login_buf);
        if (resp) {
            ESP_LOGI(TAG, "登录响应: %.200s", resp);
            free(resp);
        }

        /* 检查 token */
        if (login_buf.token[0]) {
            strncpy(g_token, login_buf.token, sizeof(g_token) - 1);
            /* 存 NVS */
            if (g_water_nvs) {
                nvs_set_str(g_water_nvs, NVS_TOKEN_KEY, g_token);
                nvs_commit(g_water_nvs);
            }
            ESP_LOGI(TAG, "登录成功: %s", g_token);
            return true;
        }

        ESP_LOGW(TAG, "登录失败 (attempt %d), 重试...", attempt + 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGE(TAG, "登录失败，已重试 10 次");
    return false;
}

/* ============================================================
 * 设备发现
 * ============================================================ */

static bool discover_machine(void)
{
    ESP_LOGI(TAG, "设备发现: 宿舍=%s", g_dorm[0] ? g_dorm : "(未配置)");
    if (!g_dorm[0]) {
        ESP_LOGE(TAG, "宿舍号未配置");
        return false;
    }

    /* 解析楼栋和房号 */
    char building[16], room[16];
    const char *dash = strchr(g_dorm, '-');
    if (dash) {
        int blen = dash - g_dorm;
        if (blen > 0 && blen < (int)sizeof(building)) {
            strncpy(building, g_dorm, blen);
            building[blen] = '\0';
        }
        strncpy(room, dash + 1, sizeof(room) - 1);
    } else {
        ESP_LOGE(TAG, "宿舍号格式错误: %s", g_dorm);
        return false;
    }

    char path[256], sign_input[256];

    /* L0+L1: 流式搜索楼栋节点（避免 cJSON_Parse 16KB 嵌套 JSON）。
     * 用 buf_try_search 在流式响应中找 "name":"<楼栋号>"，
     * 括号感知回溯正确处理 childList 嵌套。 */
    char *bnode = lxt_get_search_node(LXT_SCHOOL_ID, building);
    if (!bnode) {
        ESP_LOGE(TAG, "未找到楼栋 %s (流式搜索未命中)", building);
        return false;
    }
    ESP_LOGI(TAG, "楼栋节点: %.160s", bnode);

    cJSON *root = cJSON_Parse(bnode);
    free(bnode);
    if (!root) {
        ESP_LOGE(TAG, "楼栋节点 JSON 解析失败");
        return false;
    }

    char building_id[64] = {0};
    cJSON *b_id = cJSON_GetObjectItem(root, "id");
    if (b_id && cJSON_IsString(b_id)) {
        strncpy(building_id, b_id->valuestring, sizeof(building_id) - 1);
    }
    cJSON *b_child = cJSON_GetObjectItem(root, "childList");
    if (!building_id[0] || !cJSON_IsArray(b_child)) {
        ESP_LOGW(TAG, "楼栋节点缺 id/childList");
        cJSON_Delete(root);
        return false;
    }
    int nf = cJSON_GetArraySize(b_child);
    ESP_LOGI(TAG, "找到楼栋 id=%s, 楼层数=%d", building_id, nf);

    /* L2+L3: 遍历楼层，查找房间 */
    char room_site_id[64] = {0};
    int room_site_flag = 0;

    for (int f = 0; f < nf && !room_site_id[0]; f++) {
        cJSON *fl = cJSON_GetArrayItem(b_child, f);
        if (!fl) continue;
        cJSON *fl_id = cJSON_GetObjectItem(fl, "id");
        if (!fl_id || !cJSON_IsString(fl_id) || !fl_id->valuestring[0]) continue;
        ESP_LOGI(TAG, "L2 楼层[%d] id=%s", f, fl_id->valuestring);

        /* L3: 获取房间 */
        snprintf(path, sizeof(path),
                 "/baseDict/site/getDormitoryOrPublicRoom?areaId=%s", fl_id->valuestring);
        snprintf(sign_input, sizeof(sign_input),
                 "areaId=%s", fl_id->valuestring);
        lxt_http_buf_t rbuf;
        memset(&rbuf, 0, sizeof(rbuf));
        char *rresp = lxt_get(path, sign_input, &rbuf);
        if (!rresp) continue;

        cJSON *rr = cJSON_Parse(rresp);
        free(rresp);
        if (!rr) continue;

        cJSON *rooms = cJSON_GetObjectItem(rr, "Data");
        if (cJSON_IsArray(rooms)) {
            int nr = cJSON_GetArraySize(rooms);
            for (int r = 0; r < nr; r++) {
                cJSON *rm = cJSON_GetArrayItem(rooms, r);
                cJSON *rm_name = cJSON_GetObjectItem(rm, "name");
                cJSON *rm_id = cJSON_GetObjectItem(rm, "id");
                cJSON *rm_flag = cJSON_GetObjectItem(rm, "siteFlag");
                if (rm_name && strstr(rm_name->valuestring, room)) {
                    strncpy(room_site_id, rm_id->valuestring, sizeof(room_site_id) - 1);
                    room_site_flag = rm_flag ? rm_flag->valueint : 0;
                    ESP_LOGI(TAG, "找到房间: %s (id=%s, flag=%d)",
                             rm_name->valuestring, room_site_id, room_site_flag);
                    break;
                }
            }
        }
        cJSON_Delete(rr);
    }
    cJSON_Delete(root);

    if (!room_site_id[0]) {
        ESP_LOGE(TAG, "未找到房间 %s", room);
        return false;
    }

    /* 查找设备 */
    char post_body[256];
    snprintf(post_body, sizeof(post_body),
             "{\"siteId\":\"%s\",\"siteFlag\":%d,\"typeId\":18}",
             room_site_id, room_site_flag);

    lxt_http_buf_t dev_buf;
    char *dev_resp = lxt_post_json("/mgapp/machine/getMachineByLocation", post_body, &dev_buf);
    if (!dev_resp) return false;

    cJSON *dev_root = cJSON_Parse(dev_resp);
    free(dev_resp);
    if (!dev_root) return false;

    cJSON *dev_code = cJSON_GetObjectItem(dev_root, "Code");
    cJSON *dev_data = cJSON_GetObjectItem(dev_root, "Data");
    bool found = false;
    if (cJSON_IsNumber(dev_code) && dev_code->valueint == 0 && cJSON_IsArray(dev_data)) {
        cJSON *dev = cJSON_GetArrayItem(dev_data, 0);
        if (dev) {
            cJSON *mid = cJSON_GetObjectItem(dev, "machineId");
            if (mid && cJSON_IsString(mid)) {
                strncpy(g_machine_id, mid->valuestring, sizeof(g_machine_id) - 1);
                if (g_water_nvs) {
                    nvs_set_str(g_water_nvs, NVS_MACHINE_KEY, g_machine_id);
                    nvs_set_str(g_water_nvs, NVS_DORM_KEY, g_dorm); /* 记录宿舍号，供变化检测 */
                    nvs_commit(g_water_nvs);
                }
                ESP_LOGI(TAG, "发现设备: machineId=%s", g_machine_id);
                found = true;
            }
        }
    }
    cJSON_Delete(dev_root);
    return found;
}

/* ============================================================
 * 水费查询
 * ============================================================ */

float water_query_balance(void)
{
    if (!g_machine_id[0]) {
        ESP_LOGW(TAG, "无 machineId，需先发现设备");
        return -1.0f;
    }

    /* 参数按字母序排列 */
    char params[512];
    snprintf(params, sizeof(params),
             "investorId=%s&schoolId=%s&walletKey=%s",
             LXT_INVESTOR_ID, LXT_SCHOOL_ID, g_machine_id);

    char path[1024];
    snprintf(path, sizeof(path),
             "/paymentV1/app/wallet/find?%s", params);

    lxt_http_buf_t buf;
    char *resp = lxt_get(path, params, &buf);
    if (!resp) return -1.0f;

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) return -1.0f;

    cJSON *code = cJSON_GetObjectItem(root, "Code");
    if (cJSON_IsNumber(code) && code->valueint == -44) {
        /* Token 过期，重新登录 */
        ESP_LOGW(TAG, "Token 过期，重新登录");
        cJSON_Delete(root);
        if (water_login() && discover_machine()) {
            return water_query_balance();
        }
        return -1.0f;
    }

    cJSON *data = cJSON_GetObjectItem(root, "Data");
    float balance = -1.0f;
    if (cJSON_IsObject(data)) {
        cJSON *money = cJSON_GetObjectItem(data, "canUseMoney");
        if (cJSON_IsNumber(money)) {
            balance = money->valuedouble / 100.0f;
            ESP_LOGI(TAG, "水费余额: %.2f 元", balance);
        }
    }
    cJSON_Delete(root);
    return balance;
}

float water_query_all(void)
{
    /* 未配置乐校通账号则跳过 */
    if (!g_water_phone[0]) {
        ESP_LOGW(TAG, "乐校通账号未配置，跳过水费查询");
        return -1.0f;
    }

    /* 初始化 NVS */
    if (!g_water_nvs) {
        nvs_open("water", NVS_READWRITE, &g_water_nvs);
    }

    /* 从 NVS 加载缓存 */
    if (!g_token[0]) {
        size_t len = sizeof(g_token);
        nvs_get_str(g_water_nvs, NVS_TOKEN_KEY, g_token, &len);
    }
    if (!g_machine_id[0]) {
        size_t len = sizeof(g_machine_id);
        nvs_get_str(g_water_nvs, NVS_MACHINE_KEY, g_machine_id, &len);
    }

    /* 宿舍号变化检测（无条件）：NVS 存的宿舍号 != 当前配置，清除水表缓存重新发现 */
    if (g_dorm[0]) {
        char nvs_dorm[16] = {0};
        size_t dlen = sizeof(nvs_dorm);
        if (nvs_get_str(g_water_nvs, NVS_DORM_KEY, nvs_dorm, &dlen) == ESP_OK &&
            strcmp(nvs_dorm, g_dorm) != 0) {
            ESP_LOGW(TAG, "宿舍号变化 (%s → %s)，清除水表缓存重新发现",
                     nvs_dorm, g_dorm);
            g_machine_id[0] = '\0';
            nvs_erase_key(g_water_nvs, NVS_MACHINE_KEY);
            nvs_commit(g_water_nvs);
        }
    }

    /* 如果没有 token，登录 */
    if (!g_token[0]) {
        if (!water_login()) return -1.0f;
    }

    /* 如果没有 machineId，发现设备 */
    if (!g_machine_id[0]) {
        if (!discover_machine()) {
            /* 先不清 token，重试一次发现（可能是临时网络问题） */
            ESP_LOGW(TAG, "设备发现失败，重试一次");
            if (!discover_machine()) {
                /* 仍失败，可能 token 过期，清 token 重登 */
                ESP_LOGW(TAG, "设备发现仍失败，重登后重试");
                g_token[0] = '\0';
                g_machine_id[0] = '\0';
                if (g_water_nvs) {
                    nvs_erase_key(g_water_nvs, NVS_TOKEN_KEY);
                    nvs_erase_key(g_water_nvs, NVS_MACHINE_KEY);
                    nvs_commit(g_water_nvs);
                }
                if (!water_login()) return -1.0f;
                if (!discover_machine()) return -1.0f;
            }
        }
    }

    /* 查询 */
    return water_query_balance();
}
