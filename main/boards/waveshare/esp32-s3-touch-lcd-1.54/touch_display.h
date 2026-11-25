#pragma once

#include <functional>

#include <lvgl.h>

#include "display/lcd_display.h"
#include "lvgl_theme.h"

/**
 * SpiLcdDisplay with a touch layer on top of the standard chat UI:
 *   - double tap       : start / interrupt the conversation (same as a short PWR press)
 *   - swipe down       : open the settings panel (volume / brightness / screen timeout /
 *                        WiFi config / power off / AEC)
 *   - swipe left/right : switch between the light and the dark theme
 *
 * A single tap deliberately does nothing: with the whole 240x240 screen as the hit
 * area, every swipe that fell a few pixels short used to toggle the conversation.
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
    // Called on every touch press, before the gesture is classified: the board uses it
    // to leave power save mode without having to know anything about touch handling.
    void SetWakeHandler(std::function<void()> handler) {
        wake_handler_ = std::move(handler);
    }

    void SetupUI() override;

private:
    // LVGL event callbacks (static + user_data == this, the LVGL way)
    static void IndevEventCb(lv_event_t* e);
    static void SliderRowPressCb(lv_event_t* e);
    static void SliderRowReleasedCb(lv_event_t* e);
    static void VolumeSliderCb(lv_event_t* e);
    static void VolumeSliderValueCb(lv_event_t* e);
    static void BrightnessSliderCb(lv_event_t* e);
    static void BrightnessSliderValueCb(lv_event_t* e);
    static void BrightnessSliderReleasedCb(lv_event_t* e);
    static void SleepSliderValueCb(lv_event_t* e);
    static void SleepSliderReleasedCb(lv_event_t* e);
    static void AecButtonCb(lv_event_t* e);
    static void ConfigModeBtnCb(lv_event_t* e);
    static void ShutdownBtnCb(lv_event_t* e);

    void HandleIndevReleased(const lv_point_t& point);
    bool IsPressOnControl(const lv_point_t& point) const;
    void UpdateAecButtonColor();
    void LoadSleepMinutes(int& minutes) const;
    static void SetSliderFromPoint(lv_obj_t* slider, const lv_point_t& point);
    static void AttachSliderRow(lv_obj_t* row, lv_obj_t* slider);
    static void SetSmallFont(lv_obj_t* label);
    static void SetSliderValueLabel(lv_obj_t* label, int percent);
    static void SetSleepValueLabel(lv_obj_t* label, int minutes);
    void ToggleTheme();
    void OpenSettingBox();
    void CloseSettingBox();
    lv_obj_t* CreateRow(lv_obj_t* parent, int32_t height);
    lv_obj_t* CreateButton(lv_obj_t* parent, const char* text, lv_color_t color,
                           lv_event_cb_t cb, lv_event_code_t trigger);

    lv_obj_t* setting_box_ = nullptr;
    lv_obj_t* button_row_ = nullptr;
    lv_obj_t* volume_row_ = nullptr;
    lv_obj_t* brightness_row_ = nullptr;
    lv_obj_t* sleep_row_ = nullptr;
    lv_obj_t* volume_slider_ = nullptr;
    lv_obj_t* brightness_slider_ = nullptr;
    lv_obj_t* sleep_slider_ = nullptr;
    lv_obj_t* volume_value_label_ = nullptr;
    lv_obj_t* brightness_value_label_ = nullptr;
    lv_obj_t* sleep_value_label_ = nullptr;
    lv_obj_t* aec_button_ = nullptr;

    lv_point_t press_point_ = {};
    lv_point_t last_tap_point_ = {};
    uint32_t press_tick_ = 0;
    uint32_t last_tap_tick_ = 0;
    bool press_on_control_ = false;
    bool has_last_tap_ = false;
    bool touch_events_registered_ = false;

    std::function<void()> config_mode_handler_;
    std::function<void()> shutdown_handler_;
    std::function<void()> wake_handler_;
};
