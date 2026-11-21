#include "touch_display.h"

#include <esp_log.h>

#include "application.h"
#include "board.h"
#include "settings.h"

#define TAG "TouchDisplay"

namespace {
// 240x240 panel: a tap must stay well below the swipe threshold and be short.
constexpr int32_t kTapMaxDistance = 15;
constexpr uint32_t kTapMaxDurationMs = 700;
constexpr int32_t kSwipeMinDistance = 40;

constexpr int32_t kSettingRowHeight = 44;
constexpr int32_t kSettingPanelPadding = 6;
constexpr int32_t kSliderWidth = 160;
constexpr int32_t kSliderHeight = 14;
constexpr int32_t kKnobSize = 22;
constexpr int32_t kSliderExtClickArea = 16;
constexpr int32_t kControlHitMargin = 12;

bool PointInObj(const lv_obj_t* obj, const lv_point_t& point, int32_t margin) {
    if (obj == nullptr) {
        return false;
    }
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    return point.x >= area.x1 - margin && point.x <= area.x2 + margin &&
           point.y >= area.y1 - margin && point.y <= area.y2 + margin;
}
}  // namespace

TouchLcdDisplay::TouchLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                                 int width, int height, int offset_x, int offset_y, bool mirror_x,
                                 bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                    swap_xy) {
}

TouchLcdDisplay::~TouchLcdDisplay() = default;

void TouchLcdDisplay::SetupUI() {
    SpiLcdDisplay::SetupUI();

    DisplayLockGuard lock(this);
    lv_obj_t* screen = lv_screen_active();
    if (screen == nullptr || touch_events_registered_) {
        return;
    }

    // The chat UI scrolls; without this the vertical swipe would scroll instead of
    // reaching us as a gesture.
    lv_obj_set_scroll_dir(screen, LV_DIR_NONE);

    // Every touch is watched at indev level, so widgets on top of the chat labels
    // do not have to bubble anything to us.
    lv_indev_t* indev = lv_indev_get_next(nullptr);
    while (indev != nullptr) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_add_event_cb(indev, IndevEventCb, LV_EVENT_PRESSED, this);
            lv_indev_add_event_cb(indev, IndevEventCb, LV_EVENT_RELEASED, this);
        }
        indev = lv_indev_get_next(indev);
    }

    touch_events_registered_ = true;
}

void TouchLcdDisplay::IndevEventCb(lv_event_t* e) {
    auto self = static_cast<TouchLcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr) {
        return;
    }

    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        lv_indev_get_point(indev, &self->press_point_);
        self->press_tick_ = lv_tick_get();
        self->press_on_control_ = self->IsPressOnControl(self->press_point_);
        return;
    }

    lv_point_t point;
    lv_indev_get_point(indev, &point);
    self->HandleIndevReleased(point);
}

void TouchLcdDisplay::HandleIndevReleased(const lv_point_t& point) {
    int32_t dx = point.x - press_point_.x;
    int32_t dy = point.y - press_point_.y;
    int32_t adx = LV_ABS(dx);
    int32_t ady = LV_ABS(dy);
    uint32_t elapsed = lv_tick_elaps(press_tick_);

    // A drag that started on a slider, a switch or a button belongs to that widget.
    if (press_on_control_) {
        return;
    }

    bool panel_open = (setting_box_ != nullptr);

    if (adx < kTapMaxDistance && ady < kTapMaxDistance) {
        if (panel_open || elapsed > kTapMaxDurationMs) {
            return;
        }
        ESP_LOGI(TAG, "Tap at (%d,%d), toggle chat", point.x, point.y);
        Application::GetInstance().ToggleChatState();
        return;
    }

    if (adx < kSwipeMinDistance && ady < kSwipeMinDistance) {
        return;  // neither a tap nor a long enough swipe
    }

    if (adx >= ady) {
        // Horizontal swipe. Inside the panel it does nothing: it used to be the
        // reason a slider drag switched the theme.
        if (panel_open) {
            return;
        }
        ESP_LOGI(TAG, "Swipe %s, switch theme", dx > 0 ? "right" : "left");
        ToggleTheme();
        return;
    }

    if (dy > 0) {
        if (panel_open) {
            ESP_LOGI(TAG, "Swipe down, close settings");
            CloseSettingBox();
        } else {
            ESP_LOGI(TAG, "Swipe down, open settings");
            OpenSettingBox();
        }
        return;
    }

    // Swipe up is deliberately ignored: a tap already toggles the chat.
}

bool TouchLcdDisplay::IsPressOnControl(const lv_point_t& point) const {
    if (setting_box_ == nullptr) {
        return false;
    }
    // The whole slider row is ours: LVGL only reacts on the knob, so the row is the
    // surface that turns a press anywhere on the track into a value change.
    if (PointInObj(volume_row_, point, 0)) {
        return true;
    }
    if (PointInObj(brightness_row_, point, 0)) {
        return true;
    }
    if (PointInObj(volume_slider_, point, kControlHitMargin)) {
        return true;
    }
    if (PointInObj(brightness_slider_, point, kControlHitMargin)) {
        return true;
    }
    if (PointInObj(aec_switch_, point, kControlHitMargin)) {
        return true;
    }
    return PointInObj(button_row_, point, 0);
}

void TouchLcdDisplay::SetSliderFromPoint(lv_obj_t* slider, const lv_point_t& point) {
    if (slider == nullptr) {
        return;
    }
    lv_area_t area;
    lv_obj_get_coords(slider, &area);
    int32_t width = area.x2 - area.x1;
    if (width <= 0) {
        return;
    }
    int32_t minimum = lv_slider_get_min_value(slider);
    int32_t maximum = lv_slider_get_max_value(slider);
    int32_t offset = LV_CLAMP(point.x - area.x1, 0, width);
    int32_t value = minimum + (int32_t)(((int64_t)(maximum - minimum) * offset) / width);
    if (value == lv_slider_get_value(slider)) {
        return;
    }
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    // LVGL only changes the value while the knob itself is dragged, so a press on the
    // track has to publish the change for the handlers attached to the slider.
    lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, nullptr);
}

void TouchLcdDisplay::SliderRowPressCb(lv_event_t* e) {
    auto slider = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
    lv_indev_t* indev = lv_indev_active();
    if (slider == nullptr || indev == nullptr) {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    SetSliderFromPoint(slider, point);
}

void TouchLcdDisplay::SliderRowReleasedCb(lv_event_t* e) {
    auto slider = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
    if (slider == nullptr) {
        return;
    }
    // Let the slider handlers commit the value (volume writes NVS, brightness persists).
    lv_obj_send_event(slider, LV_EVENT_RELEASED, nullptr);
}

void TouchLcdDisplay::ToggleTheme() {
    LvglTheme* light = LvglThemeManager::GetInstance().GetTheme("light");
    LvglTheme* dark = LvglThemeManager::GetInstance().GetTheme("dark");
    if (light == nullptr || dark == nullptr) {
        return;
    }
    LvglTheme* target = (current_theme_ == light) ? dark : light;
    ESP_LOGI(TAG, "Switch theme to %s", target->name().c_str());
    SetTheme(target);
}

void TouchLcdDisplay::OpenSettingBox() {
    DisplayLockGuard lock(this);
    if (setting_box_ != nullptr) {
        return;
    }

    auto theme = static_cast<LvglTheme*>(current_theme_);
    if (theme == nullptr) {
        return;
    }
    const lv_font_t* text_font = theme->text_font()->font();

    setting_box_ = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(setting_box_);
    lv_obj_set_size(setting_box_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(setting_box_, theme->background_color(), 0);
    lv_obj_set_style_bg_opa(setting_box_, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(setting_box_, theme->text_color(), 0);
    lv_obj_set_style_text_font(setting_box_, text_font, 0);
    lv_obj_set_style_pad_all(setting_box_, kSettingPanelPadding, 0);
    lv_obj_set_style_pad_row(setting_box_, 4, 0);
    lv_obj_set_flex_flow(setting_box_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(setting_box_, LV_DIR_VER);

    // Title
    lv_obj_t* title = lv_label_create(setting_box_);
    lv_label_set_text(title, "设 置");
    lv_obj_set_width(title, lv_pct(100));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

#if CONFIG_USE_DEVICE_AEC
    {
        lv_obj_t* row = CreateRow(setting_box_, kSettingRowHeight);
        lv_obj_t* label = lv_label_create(row);
        lv_label_set_text(label, "回声消除");
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 4, 0);

        aec_switch_ = lv_switch_create(row);
        lv_obj_set_size(aec_switch_, 44, 24);
        lv_obj_align(aec_switch_, LV_ALIGN_RIGHT_MID, -4, 0);
        if (Application::GetInstance().GetAecMode() != kAecOff) {
            lv_obj_add_state(aec_switch_, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(aec_switch_, AecSwitchCb, LV_EVENT_VALUE_CHANGED, this);
    }
#endif

    {
        volume_row_ = CreateRow(setting_box_, kSettingRowHeight);
        lv_obj_t* label = lv_label_create(volume_row_);
        lv_label_set_text(label, "音量");
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 4, 0);

        auto codec = Board::GetInstance().GetAudioCodec();
        int volume = (codec != nullptr) ? codec->output_volume() : 70;

        volume_slider_ = lv_slider_create(volume_row_);
        lv_obj_set_size(volume_slider_, kSliderWidth, kSliderHeight);
        lv_obj_align(volume_slider_, LV_ALIGN_RIGHT_MID, -4, 0);
        lv_obj_set_style_width(volume_slider_, kKnobSize, LV_PART_KNOB);
        lv_obj_set_style_height(volume_slider_, kKnobSize, LV_PART_KNOB);
        lv_obj_set_style_radius(volume_slider_, LV_RADIUS_CIRCLE, LV_PART_KNOB);
        lv_slider_set_range(volume_slider_, 0, 100);
        lv_slider_set_value(volume_slider_, volume, LV_ANIM_OFF);
        lv_obj_set_ext_click_area(volume_slider_, kSliderExtClickArea);
        // Applied on release only: SetOutputVolume() writes NVS on every call.
        lv_obj_add_event_cb(volume_slider_, VolumeSliderCb, LV_EVENT_RELEASED, this);
        AttachSliderRow(volume_row_, volume_slider_);
    }

    {
        brightness_row_ = CreateRow(setting_box_, kSettingRowHeight);
        lv_obj_t* label = lv_label_create(brightness_row_);
        lv_label_set_text(label, "亮度");
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 4, 0);

        auto backlight = Board::GetInstance().GetBacklight();
        int brightness = 50;
        if (backlight != nullptr) {
            auto settings = Settings("display", false);
            brightness = settings.GetInt("brightness", 50);
            if (brightness <= 0) {
                brightness = 50;
            }
        }

        brightness_slider_ = lv_slider_create(brightness_row_);
        lv_obj_set_size(brightness_slider_, kSliderWidth, kSliderHeight);
        lv_obj_align(brightness_slider_, LV_ALIGN_RIGHT_MID, -4, 0);
        lv_obj_set_style_width(brightness_slider_, kKnobSize, LV_PART_KNOB);
        lv_obj_set_style_height(brightness_slider_, kKnobSize, LV_PART_KNOB);
        lv_obj_set_style_radius(brightness_slider_, LV_RADIUS_CIRCLE, LV_PART_KNOB);
        lv_slider_set_range(brightness_slider_, 1, 100);
        lv_slider_set_value(brightness_slider_, brightness, LV_ANIM_OFF);
        lv_obj_set_ext_click_area(brightness_slider_, kSliderExtClickArea);
        lv_obj_add_event_cb(brightness_slider_, BrightnessSliderCb, LV_EVENT_VALUE_CHANGED, this);
        lv_obj_add_event_cb(brightness_slider_, BrightnessSliderReleasedCb, LV_EVENT_RELEASED,
                            this);
        AttachSliderRow(brightness_row_, brightness_slider_);
    }

    {
        button_row_ = CreateRow(setting_box_, kSettingRowHeight);
        lv_obj_set_flex_flow(button_row_, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(button_row_, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        CreateButton(button_row_, "配网", lv_color_hex(0x2E86DE), ConfigModeBtnCb);
        CreateButton(button_row_, "关机", lv_color_hex(0xE74C3C), ShutdownBtnCb);
        CreateButton(button_row_, "关闭", lv_color_hex(0x7F8C8D), CloseBtnCb);
    }
}

void TouchLcdDisplay::CloseSettingBox() {
    DisplayLockGuard lock(this);
    if (setting_box_ == nullptr) {
        return;
    }
    // Deleting synchronously would free the object we may still be dispatching an
    // event for, so hand it to LVGL's deferred delete.
    lv_obj_delete_async(setting_box_);
    setting_box_ = nullptr;
    button_row_ = nullptr;
    volume_row_ = nullptr;
    brightness_row_ = nullptr;
    volume_slider_ = nullptr;
    brightness_slider_ = nullptr;
    aec_switch_ = nullptr;
}

void TouchLcdDisplay::AttachSliderRow(lv_obj_t* row, lv_obj_t* slider) {
    if (row == nullptr || slider == nullptr) {
        return;
    }
    // The row is the hit surface: LVGL's slider only reacts on the knob, so pressing
    // the track would otherwise do nothing at all.
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, SliderRowPressCb, LV_EVENT_PRESSED, slider);
    lv_obj_add_event_cb(row, SliderRowPressCb, LV_EVENT_PRESSING, slider);
    lv_obj_add_event_cb(row, SliderRowReleasedCb, LV_EVENT_RELEASED, slider);
}

lv_obj_t* TouchLcdDisplay::CreateRow(lv_obj_t* parent, int32_t height) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, height);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

lv_obj_t* TouchLcdDisplay::CreateButton(lv_obj_t* parent, const char* text, lv_color_t color,
                                        lv_event_cb_t cb) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_set_size(button, 68, 36);
    lv_obj_set_style_bg_color(button, color, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(button, 6, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, this);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(label);
    return button;
}

void TouchLcdDisplay::VolumeSliderCb(lv_event_t* e) {
    lv_obj_t* slider = lv_event_get_target_obj(e);
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        return;
    }
    int volume = lv_slider_get_value(slider);
    ESP_LOGI(TAG, "Set volume %d", volume);
    codec->SetOutputVolume(volume);
}

void TouchLcdDisplay::BrightnessSliderCb(lv_event_t* e) {
    lv_obj_t* slider = lv_event_get_target_obj(e);
    auto backlight = Board::GetInstance().GetBacklight();
    if (backlight == nullptr) {
        return;
    }
    backlight->SetBrightness(static_cast<uint8_t>(lv_slider_get_value(slider)));
}

void TouchLcdDisplay::BrightnessSliderReleasedCb(lv_event_t* e) {
    lv_obj_t* slider = lv_event_get_target_obj(e);
    int brightness = lv_slider_get_value(slider);
    if (brightness <= 0) {
        return;  // 0 is treated as "invalid" by RestoreBrightness()
    }
    auto settings = Settings("display", true);
    settings.SetInt("brightness", brightness);
}

void TouchLcdDisplay::AecSwitchCb(lv_event_t* e) {
    lv_obj_t* sw = lv_event_get_target_obj(e);
    bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    auto& app = Application::GetInstance();
    app.SetAecMode(enabled ? kAecOnDeviceSide : kAecOff);
    ESP_LOGI(TAG, "AEC %s", enabled ? "on" : "off");
}

void TouchLcdDisplay::ConfigModeBtnCb(lv_event_t* e) {
    auto self = static_cast<TouchLcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr || !self->config_mode_handler_) {
        return;
    }
    ESP_LOGI(TAG, "Enter WiFi configuration mode");
    self->CloseSettingBox();
    self->config_mode_handler_();
}

void TouchLcdDisplay::ShutdownBtnCb(lv_event_t* e) {
    auto self = static_cast<TouchLcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr || !self->shutdown_handler_) {
        return;
    }
    ESP_LOGI(TAG, "Power off");
    self->shutdown_handler_();
}

void TouchLcdDisplay::CloseBtnCb(lv_event_t* e) {
    auto self = static_cast<TouchLcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    self->CloseSettingBox();
}
