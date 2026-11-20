// RlcdLcdDisplay：ST7305 1bpp 反射屏的通用 LVGL 显示层
//
// 参考实现来自 waveshare/esp32-s3-rlcd-4.2 的 CustomLcdDisplay：panel_io/panel
// 传 NULL，由 RlcdDriver 自建 SPI 与 1-bit 帧缓冲，LVGL 只负责把 RGB565 渲染
// 结果逐点二值化后写进驱动。抽到 boards/common 是为了让同款屏的板卡
// （waveshare/esp32-s3-rlcd-4.2、luobo/esp32-s3-nano-rlcd）共用一份代码。

#include "rlcd_display.h"

#include <cassert>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>

static const char* TAG = "RlcdDisplay";

// ===== LVGL flush 回调：RGB565 → 1-bit =====

void RlcdLcdDisplay::Lvgl_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* color_p)
{
    assert(disp != NULL);
    RlcdLcdDisplay* self = (RlcdLcdDisplay*)lv_display_get_user_data(disp);
    RlcdDriver* rlcd = self->rlcd_;
    uint16_t* buffer = (uint16_t*)color_p;
    for (int y = area->y1; y <= area->y2; y++) {
        for (int x = area->x1; x <= area->x2; x++) {
            const uint16_t px = *buffer++;
            // RGB565 -> 亮度（0..255 近似，权重和为 256 避免除法）
            const uint16_t r5 = (px >> 11) & 0x1F;
            const uint16_t g6 = (px >> 5) & 0x3F;
            const uint16_t b5 = px & 0x1F;
            const uint16_t luma = (r5 * 616 + g6 * 600 + b5 * 232) >> 8;

            // 1-bit 硬二值化，阈值 160（约 63% 亮度）：
            // 偏向保留深色笔画，让 4bpp 抗锯齿小字的边缘尽量连成实线。
            // 不用 Bayer 抖动——抖动会把字体的灰度边缘打散成网点，小字明显发虚。
            const uint8_t color = (luma < 160) ? ColorBlack : ColorWhite;
            rlcd->RLCD_SetPixel(x, y, color);
        }
    }
    rlcd->RLCD_Display();
    lv_display_flush_ready(disp);
}

// ===== 构造 / 析构 =====

RlcdLcdDisplay::RlcdLcdDisplay(esp_lcd_panel_io_handle_t panel_io,
                               esp_lcd_panel_handle_t panel, int width, int height,
                               spi_display_config_t spiconfig, spi_host_device_t spi_host,
                               int spi_clock_hz)
    : LcdDisplay(panel_io, panel, width, height)
{
    // 1. 初始化 RLCD 硬件驱动（SPI 总线 + 面板 IO + 1-bit 帧缓冲 + LUT）
    rlcd_ = new RlcdDriver(spiconfig, width, height, spi_host, spi_clock_hz);

    // 2. 初始化 LVGL
    ESP_LOGI(TAG, "初始化 LVGL");
    lv_init();
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 2;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);
    lvgl_port_lock(0);

    const int transfer = width * height;
    display_ = lv_display_create(width, height);
    lv_display_set_flush_cb(display_, Lvgl_flush_cb);
    lv_display_set_user_data(display_, this);
    const size_t lvgl_buffer_size = LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565) * transfer;
    uint8_t* lvgl_buffer1 = (uint8_t*)heap_caps_malloc(lvgl_buffer_size, MALLOC_CAP_SPIRAM);
    assert(lvgl_buffer1);
    lv_display_set_buffers(display_, lvgl_buffer1, NULL, lvgl_buffer_size,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    // 3. 初始化 RLCD 屏幕（发初始化命令序列 + 清屏）
    ESP_LOGI(TAG, "初始化 RLCD 屏幕 %dx%d @%dHz", width, height, spi_clock_hz);
    rlcd_->RLCD_Init();

    lvgl_port_unlock();
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "显示初始化失败");
    }
}

RlcdLcdDisplay::~RlcdLcdDisplay() {
    // 基类析构会 lv_display_delete，期间可能调用 flush 回调访问 rlcd_，
    // 先把 flush 回调置空再释放驱动，避免 use-after-free
    if (display_) {
        lv_display_set_flush_cb(display_, [](lv_display_t*, const lv_area_t*, uint8_t*) {});
    }
    delete rlcd_;
    rlcd_ = nullptr;
}
