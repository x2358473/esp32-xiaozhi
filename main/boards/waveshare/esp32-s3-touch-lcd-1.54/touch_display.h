#pragma once

#include <functional>

#include <lvgl.h>

#include "display/lcd_display.h"
#include "lvgl_theme.h"

/**
 * SpiLcdDisplay with a touch layer on top of the standard chat UI:
 *   - tap              : start / interrupt the conversation (same as a short PWR press)
 *   - swipe up         : start / interrupt the conversation
 *   - swipe down       : open the settings panel (AEC / volume / brightness / WiFi config / power off)
 *   - swipe left/right : switch between the light and the dark theme
 *
 * Swipes are recognised from the indev PRESSED/RELEASED points instead of LVGL's
 * gesture events: LVGL only looks at the direction, so a horizontal drag on a
 * slider inside the panel would be reported as LV_DIR_LEFT/RIGHT. A press that
 * starts on a slider, a switch or a button row is therefore never a gesture, and
 * while the panel is open only a downward swipe closes it.
 *
 * The two actions that live outside the display layer (WiFi configuration mode and
 * power off) are injected by the board through the handler setters, so this class
 * only depends on the generic Board/Application API.
 */
class TouchLcdDisplay : public SpiLcdDisplay {
public:
    TouchLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                    int width, int height, int offset_x, int offset_y,
                    bool mirror_x, bool mirror_y, bool swap_xy);
    ~TouchLcdDisplay() override;

    void SetConfigModeHandler(std::function<void()> handler) {
        config_mode_handler_ = std::move(handler);
    }
    void SetShutdownHandler(std::function<void()> handler) {
        shutdown_handler_ = std::move(handler);
    }

    void SetupUI() override;

private:
    // LVGL event callbacks (static + user_data == this, the LVGL way)
    static void IndevEventCb(lv_event_t* e);
    static void VolumeSliderCb(lv_event_t* e);
    static void BrightnessSliderCb(lv_event_t* e);
    static void BrightnessSliderReleasedCb(lv_event_t* e);
    static void AecSwitchCb(lv_event_t* e);
    static void ConfigModeBtnCb(lv_event_t* e);
    static void ShutdownBtnCb(lv_event_t* e);
    static void CloseBtnCb(lv_event_t* e);

    void HandleIndevReleased(const lv_point_t& point);
    bool IsPressOnControl(const lv_point_t& point) const;
    void ToggleTheme();
    void OpenSettingBox();
    void CloseSettingBox();
    lv_obj_t* CreateRow(lv_obj_t* parent, int32_t height);
    lv_obj_t* CreateButton(lv_obj_t* parent, const char* text, lv_color_t color,
                           lv_event_cb_t cb);

    lv_obj_t* setting_box_ = nullptr;
    lv_obj_t* button_row_ = nullptr;
    lv_obj_t* volume_slider_ = nullptr;
    lv_obj_t* brightness_slider_ = nullptr;
    lv_obj_t* aec_switch_ = nullptr;

    lv_point_t press_point_ = {};
    uint32_t press_tick_ = 0;
    bool press_on_control_ = false;
    bool touch_events_registered_ = false;

    std::function<void()> config_mode_handler_;
    std::function<void()> shutdown_handler_;
};
