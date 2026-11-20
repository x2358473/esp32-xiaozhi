#ifndef RLCD_DISPLAY_H
#define RLCD_DISPLAY_H

#include "lcd_display.h"
#include "rlcd_driver.h"

// ST7305 400x300 1bpp 反射屏的通用显示层
//
// 这块屏不走 esp_lcd panel：RlcdDriver 自己发 SPI 命令序列、自己维护 1-bit
// 帧缓冲，因此不能复用 SpiLcdDisplay（它要真实的 panel_io/panel 来做
// esp_lcd_panel_draw_bitmap）。这里只负责 LVGL 初始化 + flush 回调接线，
// 界面完全复用 LcdDisplay 的标准布局，板卡侧只需要给引脚和 SPI 时钟。
//
// 想自定义界面的板卡可以继承本类，在 SetupUI() 里先调 RlcdLcdDisplay::SetupUI()
// 再叠加自己的控件（参考 waveshare/esp32-s3-rlcd-4.2 的 CustomLcdDisplay）。
class RlcdLcdDisplay : public LcdDisplay {
public:
    RlcdLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                   int width, int height, spi_display_config_t spiconfig,
                   spi_host_device_t spi_host = SPI3_HOST,
                   int spi_clock_hz = 40 * 1000 * 1000);
    virtual ~RlcdLcdDisplay();

private:
    static void Lvgl_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* color_p);

    RlcdDriver* rlcd_ = nullptr;
};

#endif  // RLCD_DISPLAY_H
