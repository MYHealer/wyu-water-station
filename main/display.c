/*
 * display.c - ST7789 SPI LCD + LVGL 初始化
 *
 * 骨架参考: E:\ESP\taiji-knob-fw\main\display.c (SH8601 QSPI 版本)
 * 本文件改为标准 SPI ST7789，去掉触摸/I2C/背光 PWM
 */

#include "display.h"
#include "config.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = TAG_DISP;

static SemaphoreHandle_t lvgl_mux = NULL;
static lv_disp_drv_t disp_drv;
static esp_lcd_panel_handle_t panel_handle = NULL;

static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx)
{
    lv_disp_drv_t *drv = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(drv);
    return false;
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t ph = (esp_lcd_panel_handle_t)drv->user_data;
    esp_lcd_panel_draw_bitmap(ph, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_map);
}

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static bool lvgl_lock(int timeout_ms)
{
    assert(lvgl_mux);
    const TickType_t ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(lvgl_mux, ticks) == pdTRUE;
}

static void lvgl_unlock(void)
{
    xSemaphoreGive(lvgl_mux);
}

static void lvgl_task(void *arg)
{
    ESP_LOGI(TAG, "LVGL task started");
    uint32_t delay_ms = LVGL_TASK_MAX_DELAY_MS;
    while (1) {
        if (lvgl_lock(-1)) {
            delay_ms = lv_timer_handler();
            lvgl_unlock();
        }
        if (delay_ms > LVGL_TASK_MAX_DELAY_MS) {
            delay_ms = LVGL_TASK_MAX_DELAY_MS;
        } else if (delay_ms < LVGL_TASK_MIN_DELAY_MS) {
            delay_ms = LVGL_TASK_MIN_DELAY_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "Initializing ST7789 %dx%d ...", LCD_H_RES, LCD_V_RES);

    /* 1. SPI 总线 */
    ESP_LOGI(TAG, "Init SPI bus (MOSI=%d SCLK=%d MISO=%d)",
             PIN_LCD_MOSI, PIN_LCD_SCLK, PIN_LCD_MISO);
    const spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .sclk_io_num = PIN_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LVGL_BUF_HEIGHT * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* 2. Panel IO */
    ESP_LOGI(TAG, "Install panel IO (CS=%d DC=%d)", PIN_LCD_CS, PIN_LCD_DC);
    esp_lcd_panel_io_handle_t io_handle = NULL;
    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_PIXEL_CLK,
        .trans_queue_depth = 10,
        .on_color_trans_done = notify_lvgl_flush_ready,
        .user_ctx = &disp_drv,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags = {
            .sio_mode = 1,   /* 单线 MOSI 传输 */
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                             &io_config, &io_handle));

    /* 3. ST7789 面板 */
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,   /* 实测：BGR 时红蓝互换，此屏实际为 RGB */
        .bits_per_pixel = 16,
    };
    ESP_LOGI(TAG, "Install ST7789 panel driver (RST=%d)", PIN_LCD_RST);
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    /* ST7789 多数模组需要反色显示，User_Setup.h 里是 TFT_INVERSION_OFF，
     * 若颜色异常把这里的 true 改成 false 再试 */
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));

    /* 旋转 180°：X、Y 双向镜像（MADCTL MX+MY 位） */
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, true));

    /* 清屏为黑 */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    /* 4. LVGL */
    ESP_LOGI(TAG, "Initialize LVGL");
    lv_init();

    static lv_disp_draw_buf_t disp_buf;
    const size_t buf_pixels = LCD_H_RES * LVGL_BUF_HEIGHT;
    lv_color_t *buf1 = heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(buf1);
    lv_color_t *buf2 = heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(buf2);
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, buf_pixels);

    ESP_LOGI(TAG, "Register display driver (buf %u px x2)", (unsigned)buf_pixels);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;
    lv_disp_drv_register(&disp_drv);

    /* 5. LVGL tick 定时器 */
    const esp_timer_create_args_t tick_args = {
        .callback = &lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    /* 6. 互斥锁 + 任务 */
    lvgl_mux = xSemaphoreCreateMutex();
    assert(lvgl_mux);
    xTaskCreate(lvgl_task, "LVGL", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL);

    ESP_LOGI(TAG, "Display initialized: %dx%d", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}

bool display_lvgl_lock(int timeout_ms)
{
    return lvgl_lock(timeout_ms);
}

void display_lvgl_unlock(void)
{
    lvgl_unlock();
}
