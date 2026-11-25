#include "touch_panel.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>

#include "application.h"
#include "audio_codec.h"
#include "backlight.h"
#include "board.h"
#include "settings.h"

#define TAG "TouchPanel"

namespace cyberai {
namespace {

// The page fills the whole 360x360 panel so no part of the home screen can stay
// visible. The active area of this round display is a circle of roughly 160 px
// radius centred at (180,180), so every control is kept inside that circle: rows
// near the top and bottom are narrow, the button row sits near the vertical centre
// where the circle is widest.
constexpr int kPanelW = 360;
constexpr int kPanelH = 360;

constexpr int kTrackX = 55;
constexpr int kTrackW = 250;
constexpr int kTrackH = 10;
constexpr int kKnobW = 14;
constexpr int kKnobH = 18;

constexpr int kBrightnessTrackY = 116;
constexpr int kVolumeTrackY = 166;
constexpr int kSliderBand = 18;  // Half height of the per-row drag band.

constexpr int kButtonY = 186;
constexpr int kButtonH = 30;
constexpr int kButtonW = 96;
constexpr int kButtonAecX = 28;
constexpr int kButtonWifiX = 132;
constexpr int kButtonCloseX = 236;

// Anti-mistouch limits.
constexpr int kBrightnessMin = 5;   // Never let the user dim the screen to black.
constexpr int kVolumeMin = 0;
constexpr int kDragStartSlop = 6;   // A plain tap must never change a slider.
constexpr int kSwipeDy = 70;
constexpr int kPullStartY = 120;    // Pull-down must start near the top of the screen.
constexpr int kDoubleTapSlop = 30;
constexpr int kTapMoveSlop = 20;
constexpr int64_t kDoubleTapMs = 380;
constexpr int64_t kTapMaxMs = 700;
constexpr int64_t kIdleCloseMs = 10000;
// emote re-shows the eye animation and the idle hints whenever the device state or
// emotion changes, so the page re-asserts "scene hidden" for as long as it is open.
constexpr int kGuardIntervalMs = 120;

constexpr uint32_t kColorPanel = 0x141821;
constexpr uint32_t kColorTrack = 0x2C3240;
constexpr uint32_t kColorBrightness = 0xFFC447;
constexpr uint32_t kColorVolume = 0x4FC3F7;
constexpr uint32_t kColorKnob = 0xFFFFFF;
constexpr uint32_t kColorText = 0xF2F4F8;
constexpr uint32_t kColorTextDim = 0x9AA4B8;
constexpr uint32_t kColorButton = 0x2A3140;
constexpr uint32_t kColorAecOn = 0x2E7D32;
constexpr uint32_t kColorAecOff = 0x5A3038;

// A label with an opaque background is the cheapest solid rectangle this renderer
// offers: gfx_draw_label fills the object area with the background colour before it
// blends any glyph. The single space keeps the text path alive (an empty text makes
// the draw call bail out early), and the text colour matches the fill so it is
// invisible.
void ConfigureBox(gfx_obj_t* obj, int x, int y, int w, int h, uint32_t color) {
    if (obj == nullptr) {
        return;
    }
    gfx_label_set_long_mode(obj, GFX_LABEL_LONG_CLIP);
    gfx_label_set_text_align(obj, GFX_TEXT_ALIGN_CENTER);
    gfx_label_set_text(obj, " ");
    gfx_label_set_color(obj, GFX_COLOR_HEX(color));
    gfx_label_set_bg_color(obj, GFX_COLOR_HEX(color));
    gfx_label_set_bg_enable(obj, true);
    gfx_obj_set_pos(obj, x, y);
    gfx_obj_set_size(obj, static_cast<uint16_t>(w), static_cast<uint16_t>(h));
}

void ConfigureText(gfx_obj_t* obj, int x, int y, int w, int h, uint32_t color,
                   gfx_text_align_t align) {
    if (obj == nullptr) {
        return;
    }
    gfx_label_set_long_mode(obj, GFX_LABEL_LONG_CLIP);
    gfx_label_set_text_align(obj, align);
    gfx_label_set_color(obj, GFX_COLOR_HEX(color));
    // Opaque panel-coloured background: every text row then own its rectangle, so a
    // stale scene pixel can never show through a glyph gap, and the rectangle itself
    // marks exactly where the row ended up on screen.
    gfx_label_set_bg_color(obj, GFX_COLOR_HEX(kColorPanel));
    gfx_label_set_bg_enable(obj, true);
    gfx_obj_set_pos(obj, x, y);
    gfx_obj_set_size(obj, static_cast<uint16_t>(w), static_cast<uint16_t>(h));
}

int BrightnessToPixels(int value) {
    const int span = 100 - kBrightnessMin;
    return (value - kBrightnessMin) * kTrackW / span;
}

int VolumeToPixels(int value) { return value * kTrackW / 100; }

}  // namespace

TouchPanel::TouchPanel(emote_handle_t emote_handle, Backlight* backlight)
    : emote_(emote_handle), backlight_(backlight) {}

TouchPanel::~TouchPanel() {
    if (guard_timer_ != nullptr) {
        esp_timer_stop(guard_timer_);
        esp_timer_delete(guard_timer_);
        guard_timer_ = nullptr;
    }
    if (idle_timer_ != nullptr) {
        esp_timer_stop(idle_timer_);
        esp_timer_delete(idle_timer_);
        idle_timer_ = nullptr;
    }
    if (emote_ != nullptr && ready_) {
        Lock();
        SetVisibleLocked(false);
        Unlock();
    }
}

void TouchPanel::Lock() const {
    if (emote_ != nullptr) {
        emote_lock(emote_);
    }
}

void TouchPanel::Unlock() const {
    if (emote_ != nullptr) {
        emote_unlock(emote_);
    }
}

bool TouchPanel::CreateObjects() {
    bg_ = emote_create_obj_by_type(emote_, EMOTE_OBJ_TYPE_LABEL, "tp_bg");
    title_ = emote_create_obj_by_type(emote_, EMOTE_OBJ_TYPE_LABEL, "tp_title");
    brightness_caption_ = emote_create_obj_by_type(emote_, EMOTE_OBJ_TYPE_LABEL, "tp_bright_txt");
    brightness_track_ = emote_create_obj_by_type(emote_, EMOTE_OBJ_TYPE_LABEL, "tp_bright_track");
    brightness_fill_ = emote_create_obj_by_type(emote_, EMOTE_OBJ_TYPE_LABEL, "tp_bright_fill");
    brightness_knob_ = emote_create_obj_by_type(emote_, EMOTE_OBJ_TYPE_LABEL, "tp_bright_knob");
    volume_caption_ = emote_create_obj_by_type(emote_, EMOTE_OBJ_TYPE_LABEL, "tp_vol_txt");
    volume_track_ = emote_create_obj_by_type(emote_, EMOTE_OBJ_TYPE_LABEL, "tp_vol_track");
    volume_fill_ = emote_create_obj_by_type(emote_, EMOTE_OBJ_TYPE_LABEL, "tp_vol_fill");
    volume_knob_ = emote_create_obj_by_type(emote_, EMOTE_OBJ_TYPE_LABEL, "tp_vol_knob");
    wifi_label_ = emote_create_obj_by_type(emote_, EMOTE_OBJ_TYPE_LABEL, "tp_wifi");
    ip_label_ = emote_create_obj_by_type(emote_, EMOTE_OBJ_TYPE_LABEL, "tp_ip");
    aec_button_ = emote_create_obj_by_type(emote_, EMOTE_OBJ_TYPE_LABEL, "tp_btn_aec");
    wifi_button_ = emote_create_obj_by_type(emote_, EMOTE_OBJ_TYPE_LABEL, "tp_btn_wifi");
    close_button_ = emote_create_obj_by_type(emote_, EMOTE_OBJ_TYPE_LABEL, "tp_btn_close");

    return bg_ != nullptr && title_ != nullptr && brightness_caption_ != nullptr &&
           brightness_track_ != nullptr && brightness_fill_ != nullptr &&
           brightness_knob_ != nullptr && volume_caption_ != nullptr &&
           volume_track_ != nullptr && volume_fill_ != nullptr && volume_knob_ != nullptr &&
           wifi_label_ != nullptr && ip_label_ != nullptr && aec_button_ != nullptr &&
           wifi_button_ != nullptr && close_button_ != nullptr;
}

bool TouchPanel::Initialize() {
    if (emote_ == nullptr) {
        ESP_LOGE(TAG, "No emote handle, settings drawer disabled");
        return false;
    }

    Lock();
    const bool created = CreateObjects();
    if (created) {
        ConfigureBox(bg_, 0, 0, kPanelW, kPanelH, kColorPanel);

        ConfigureText(title_, 60, 44, 240, 30, kColorText, GFX_TEXT_ALIGN_CENTER);
        gfx_label_set_text(title_, "设置");

        ConfigureText(brightness_caption_, 60, 86, 240, 24, kColorText, GFX_TEXT_ALIGN_LEFT);
        ConfigureBox(brightness_track_, kTrackX, kBrightnessTrackY, kTrackW, kTrackH, kColorTrack);
        ConfigureBox(brightness_fill_, kTrackX, kBrightnessTrackY, 1, kTrackH, kColorBrightness);
        ConfigureBox(brightness_knob_, kTrackX, kBrightnessTrackY - 4, kKnobW, kKnobH, kColorKnob);

        ConfigureText(volume_caption_, 60, 136, 240, 24, kColorText, GFX_TEXT_ALIGN_LEFT);
        ConfigureBox(volume_track_, kTrackX, kVolumeTrackY, kTrackW, kTrackH, kColorTrack);
        ConfigureBox(volume_fill_, kTrackX, kVolumeTrackY, 1, kTrackH, kColorVolume);
        ConfigureBox(volume_knob_, kTrackX, kVolumeTrackY - 4, kKnobW, kKnobH, kColorKnob);

        ConfigureText(wifi_label_, 60, 228, 240, 22, kColorTextDim, GFX_TEXT_ALIGN_LEFT);
        ConfigureText(ip_label_, 60, 250, 240, 22, kColorTextDim, GFX_TEXT_ALIGN_LEFT);

        ConfigureText(aec_button_, kButtonAecX, kButtonY, kButtonW, kButtonH, kColorText,
                      GFX_TEXT_ALIGN_CENTER);
        gfx_label_set_bg_enable(aec_button_, true);
        gfx_label_set_bg_color(aec_button_, GFX_COLOR_HEX(kColorButton));

        ConfigureText(wifi_button_, kButtonWifiX, kButtonY, kButtonW, kButtonH, kColorText,
                      GFX_TEXT_ALIGN_CENTER);
        gfx_label_set_bg_enable(wifi_button_, true);
        gfx_label_set_bg_color(wifi_button_, GFX_COLOR_HEX(kColorButton));
        gfx_label_set_text(wifi_button_, "配网");

        ConfigureText(close_button_, kButtonCloseX, kButtonY, kButtonW, kButtonH, kColorText,
                      GFX_TEXT_ALIGN_CENTER);
        gfx_label_set_bg_enable(close_button_, true);
        gfx_label_set_bg_color(close_button_, GFX_COLOR_HEX(kColorButton));
        gfx_label_set_text(close_button_, "关闭");

        LogGeometryLocked();
        SetVisibleLocked(false);
    }
    Unlock();

    if (!created) {
        ESP_LOGE(TAG, "Failed to create settings drawer objects");
        return false;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = &TouchPanel::IdleTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "tp_idle",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&timer_args, &idle_timer_) != ESP_OK) {
        ESP_LOGW(TAG, "Idle timer unavailable, drawer will not auto-close");
        idle_timer_ = nullptr;
    }

    const esp_timer_create_args_t guard_args = {
        .callback = &TouchPanel::GuardTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "tp_guard",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&guard_args, &guard_timer_) != ESP_OK) {
        ESP_LOGW(TAG, "Guard timer unavailable, emote may draw over the page");
        guard_timer_ = nullptr;
    }

    ready_ = true;
    ESP_LOGI(TAG, "Settings drawer ready");
    return true;
}

void TouchPanel::SetVisibleLocked(bool visible) {
    gfx_obj_t* objects[] = {bg_,       title_,           brightness_caption_,
                            brightness_track_, brightness_knob_, volume_caption_,
                            volume_track_, volume_knob_,  wifi_label_,
                            ip_label_, aec_button_,      wifi_button_,
                            close_button_};
    for (gfx_obj_t* obj : objects) {
        gfx_obj_set_visible(obj, visible);
    }
    // Slider fills follow their value, so they are handled by the render helpers.
    gfx_obj_set_visible(brightness_fill_, false);
    gfx_obj_set_visible(volume_fill_, false);
}

void TouchPanel::RenderBrightnessLocked() {
    const int pixels = BrightnessToPixels(brightness_);
    gfx_label_set_text_fmt(brightness_caption_, "亮度 %d%%", brightness_);
    gfx_obj_set_size(brightness_fill_, static_cast<uint16_t>(std::max(pixels, 1)),
                     static_cast<uint16_t>(kTrackH));
    gfx_obj_set_visible(brightness_fill_, pixels > 0);
    gfx_obj_set_pos(brightness_knob_, kTrackX + pixels - kKnobW / 2,
                    kBrightnessTrackY - 4);
}

void TouchPanel::RenderVolumeLocked() {
    const int pixels = VolumeToPixels(volume_);
    gfx_label_set_text_fmt(volume_caption_, "音量 %d%%", volume_);
    gfx_obj_set_size(volume_fill_, static_cast<uint16_t>(std::max(pixels, 1)),
                     static_cast<uint16_t>(kTrackH));
    gfx_obj_set_visible(volume_fill_, pixels > 0);
    gfx_obj_set_pos(volume_knob_, kTrackX + pixels - kKnobW / 2, kVolumeTrackY - 4);
}

void TouchPanel::RenderAecLocked() {
    gfx_label_set_text(aec_button_, aec_enabled_ ? "AEC 开" : "AEC 关");
    gfx_label_set_bg_color(aec_button_,
                           GFX_COLOR_HEX(aec_enabled_ ? kColorAecOn : kColorAecOff));
}

void TouchPanel::RenderAllLocked() {
    RenderBrightnessLocked();
    RenderVolumeLocked();
    RenderAecLocked();
}

void TouchPanel::RefreshNetworkLocked() {
    char buffer[64];
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        snprintf(buffer, sizeof(buffer), "WiFi  %s  %d dBm",
                 reinterpret_cast<const char*>(ap.ssid), ap.rssi);
    } else {
        snprintf(buffer, sizeof(buffer), "WiFi  未连接");
    }
    gfx_label_set_text(wifi_label_, buffer);

    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info = {};
    if (netif != nullptr && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK &&
        ip_info.ip.addr != 0) {
        snprintf(buffer, sizeof(buffer), "IP  " IPSTR, IP2STR(&ip_info.ip));
    } else {
        snprintf(buffer, sizeof(buffer), "IP  --");
    }
    gfx_label_set_text(ip_label_, buffer);
}

void TouchPanel::RefreshSourcesLocked() {
    if (backlight_ != nullptr) {
        const int current = backlight_->brightness();
        brightness_ = current >= kBrightnessMin ? current : 70;
        brightness_ = std::clamp(brightness_, kBrightnessMin, 100);
    }
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec != nullptr) {
        volume_ = std::clamp(codec->output_volume(), kVolumeMin, 100);
    }
    aec_enabled_ = Application::GetInstance().GetAecMode() != kAecOff;
}

void TouchPanel::ApplyDragLocked(int x) {
    const int pixels = std::clamp(x - kTrackX, 0, kTrackW);
    if (drag_target_ == DragTarget::kBrightness) {
        brightness_ = kBrightnessMin + pixels * (100 - kBrightnessMin) / kTrackW;
        brightness_ = std::clamp(brightness_, kBrightnessMin, 100);
        if (backlight_ != nullptr) {
            backlight_->SetBrightness(static_cast<uint8_t>(brightness_), false);
        }
        RenderBrightnessLocked();
    } else if (drag_target_ == DragTarget::kVolume) {
        volume_ = std::clamp(pixels * 100 / kTrackW, kVolumeMin, 100);
        RenderVolumeLocked();
    }
}

void TouchPanel::CommitDragLocked() {
    if (drag_target_ == DragTarget::kBrightness) {
        if (backlight_ != nullptr) {
            backlight_->SetBrightness(static_cast<uint8_t>(brightness_), true);
        }
        ESP_LOGI(TAG, "Brightness %d%%", brightness_);
    } else if (drag_target_ == DragTarget::kVolume) {
        // SetOutputVolume() writes NVS on every call, so it is applied once here.
        auto* codec = Board::GetInstance().GetAudioCodec();
        if (codec != nullptr) {
            codec->SetOutputVolume(volume_);
        }
        ESP_LOGI(TAG, "Volume %d%%", volume_);
    }
}

void TouchPanel::ToggleAecLocked() {
    aec_enabled_ = !aec_enabled_;
    auto& app = Application::GetInstance();
    app.SetAecMode(aec_enabled_ ? kAecOnDeviceSide : kAecOff);
    Settings settings("aec", true);
    settings.SetInt("aec_mode", aec_enabled_ ? 1 : 0);
    ESP_LOGI(TAG, "AEC %s", aec_enabled_ ? "on" : "off");
    RenderAecLocked();
}

void TouchPanel::RestartIdleTimerLocked() {
    if (idle_timer_ == nullptr) {
        return;
    }
    esp_timer_stop(idle_timer_);
    esp_timer_start_once(idle_timer_, static_cast<uint64_t>(kIdleCloseMs) * 1000ULL);
}

void TouchPanel::HideSceneLocked() {
    static const char* const kSceneNames[kSceneObjectCount] = {
        EMT_DEF_ELEM_EYE_ANIM,    EMT_DEF_ELEM_TOAST_LABEL,  EMT_DEF_ELEM_STATUS_ICON,
        EMT_DEF_ELEM_CLOCK_LABEL, EMT_DEF_ELEM_LISTEN_ANIM,  EMT_DEF_ELEM_BAT_LEFT_LABEL,
        EMT_DEF_ELEM_CHARGE_ICON, EMT_DEF_ELEM_EMERG_DLG,    EMT_DEF_ELEM_QRCODE,
        EMT_DEF_ELEM_DEFAULT_LABEL,
    };
    for (int i = 0; i < kSceneObjectCount; ++i) {
        gfx_obj_t* obj = scene_[i].obj;
        if (obj == nullptr) {
            obj = emote_get_obj_by_name(emote_, kSceneNames[i]);
            scene_[i].obj = obj;
            if (obj == nullptr) {
                continue;
            }
        }
        // Only hide what is visible right now: a redundant set_visible() would dirty
        // the object area again on every guard tick.
        if (!gfx_obj_get_visible(obj)) {
            continue;
        }
        scene_[i].hidden_by_page = true;
        gfx_obj_set_visible(obj, false);
    }
}

void TouchPanel::RestoreSceneLocked() {
    for (int i = 0; i < kSceneObjectCount; ++i) {
        if (scene_[i].obj == nullptr || !scene_[i].hidden_by_page) {
            continue;
        }
        scene_[i].hidden_by_page = false;
        gfx_obj_set_visible(scene_[i].obj, true);
    }
}

void TouchPanel::LogGeometryLocked() const {
    struct Item {
        const char* name;
        gfx_obj_t* obj;
    };
    const Item items[] = {
        {"bg", bg_},
        {"title", title_},
        {"bright_txt", brightness_caption_},
        {"bright_track", brightness_track_},
        {"bright_fill", brightness_fill_},
        {"bright_knob", brightness_knob_},
        {"vol_txt", volume_caption_},
        {"vol_track", volume_track_},
        {"vol_fill", volume_fill_},
        {"vol_knob", volume_knob_},
        {"wifi", wifi_label_},
        {"ip", ip_label_},
        {"btn_aec", aec_button_},
        {"btn_wifi", wifi_button_},
        {"btn_close", close_button_},
    };
    for (const Item& item : items) {
        if (item.obj == nullptr) {
            ESP_LOGW(TAG, "obj %s missing", item.name);
            continue;
        }
        gfx_coord_t x = 0;
        gfx_coord_t y = 0;
        uint16_t w = 0;
        uint16_t h = 0;
        gfx_obj_get_pos(item.obj, &x, &y);
        gfx_obj_get_size(item.obj, &w, &h);
        ESP_LOGI(TAG, "obj %-12s pos %3d,%3d size %3ux%3u visible %d", item.name, static_cast<int>(x),
                 static_cast<int>(y), static_cast<unsigned>(w), static_cast<unsigned>(h),
                 static_cast<int>(gfx_obj_get_visible(item.obj)));
    }
}

void TouchPanel::OpenLocked() {
    if (!ready_ || open_) {
        return;
    }
    open_ = true;
    // Take the home scene down instead of painting over it: emote owns those objects
    // and would happily draw them again on top of the page.
    HideSceneLocked();
    RefreshSourcesLocked();
    RenderAllLocked();
    RefreshNetworkLocked();
    SetVisibleLocked(true);
    gfx_obj_set_visible(brightness_fill_, BrightnessToPixels(brightness_) > 0);
    gfx_obj_set_visible(volume_fill_, VolumeToPixels(volume_) > 0);
    RestartIdleTimerLocked();
    if (guard_timer_ != nullptr) {
        esp_timer_start_periodic(guard_timer_, static_cast<uint64_t>(kGuardIntervalMs) * 1000ULL);
    }
    LogGeometryLocked();
    ESP_LOGI(TAG, "Settings page open");
}

void TouchPanel::CloseLocked() {
    if (!open_) {
        return;
    }
    open_ = false;
    if (guard_timer_ != nullptr) {
        esp_timer_stop(guard_timer_);
    }
    SetVisibleLocked(false);
    RestoreSceneLocked();
    if (idle_timer_ != nullptr) {
        esp_timer_stop(idle_timer_);
    }
    last_tap_ms_ = 0;
    ESP_LOGI(TAG, "Settings page closed");
}

void TouchPanel::GuardTimerCallback(void* arg) {
    auto* self = static_cast<TouchPanel*>(arg);
    if (self == nullptr || !self->open_) {
        return;
    }
    self->Lock();
    if (self->open_) {
        self->HideSceneLocked();
    }
    self->Unlock();
}

void TouchPanel::IdleTimerCallback(void* arg) {
    auto* self = static_cast<TouchPanel*>(arg);
    if (self == nullptr || !self->open_) {
        return;
    }
    self->Lock();
    self->CloseLocked();
    self->Unlock();
}

bool TouchPanel::HitButton(int x, int y, Button button) const {
    int left = 0;
    switch (button) {
        case Button::kAec:
            left = kButtonAecX;
            break;
        case Button::kWifi:
            left = kButtonWifiX;
            break;
        case Button::kClose:
            left = kButtonCloseX;
            break;
    }
    return x >= left && x < left + kButtonW && y >= kButtonY && y < kButtonY + kButtonH;
}

void TouchPanel::OnPress(int x, int y, int64_t now_ms) {
    Lock();
    press_x_ = x;
    press_y_ = y;
    press_ms_ = now_ms;
    last_x_ = x;
    last_y_ = y;
    drag_target_ = DragTarget::kNone;
    drag_armed_ = false;
    swiped_ = false;
    second_tap_ = false;

    if (open_) {
        if (std::abs(y - kBrightnessTrackY) <= kSliderBand) {
            drag_target_ = DragTarget::kBrightness;
        } else if (std::abs(y - kVolumeTrackY) <= kSliderBand) {
            drag_target_ = DragTarget::kVolume;
        }
    } else if (last_tap_ms_ != 0 && now_ms - last_tap_ms_ <= kDoubleTapMs &&
               std::abs(x - last_tap_x_) <= kDoubleTapSlop &&
               std::abs(y - last_tap_y_) <= kDoubleTapSlop) {
        second_tap_ = true;
    }
    if (open_) {
        // Re-assert: an emote event may have shown the face again while the page is up.
        emote_set_anim_visible(emote_, false);
        RestartIdleTimerLocked();
    }
    Unlock();
}

void TouchPanel::OnMove(int x, int y, int64_t now_ms) {
    (void)now_ms;
    Lock();
    last_x_ = x;
    last_y_ = y;
    const int dx = x - press_x_;
    const int dy = y - press_y_;

    if (drag_target_ != DragTarget::kNone) {
        if (!drag_armed_ && std::abs(dx) >= kDragStartSlop && std::abs(dx) > std::abs(dy)) {
            drag_armed_ = true;
        }
        if (drag_armed_) {
            ApplyDragLocked(x);
            RestartIdleTimerLocked();
        }
        Unlock();
        return;
    }

    if (!swiped_ && std::abs(dy) >= kSwipeDy && std::abs(dy) > std::abs(dx)) {
        if (open_ && dy < 0) {
            swiped_ = true;
            CloseLocked();
        } else if (!open_ && dy > 0 && press_y_ < kPullStartY) {
            swiped_ = true;
            OpenLocked();
        }
    }
    Unlock();
}

TouchPanel::Action TouchPanel::OnRelease(int x, int y, int64_t now_ms) {
    Lock();
    last_x_ = x;
    last_y_ = y;
    Action action = Action::kNone;
    const int dx = x - press_x_;
    const int dy = y - press_y_;
    const bool moved = std::abs(dx) > kTapMoveSlop || std::abs(dy) > kTapMoveSlop;
    const int64_t duration = now_ms - press_ms_;

    if (open_) {
        if (drag_target_ != DragTarget::kNone) {
            if (drag_armed_) {
                CommitDragLocked();
            }
        } else if (!swiped_ && !moved) {
            if (HitButton(x, y, Button::kAec)) {
                ToggleAecLocked();
            } else if (HitButton(x, y, Button::kWifi)) {
                action = Action::kWifiConfig;
                CloseLocked();
            } else if (HitButton(x, y, Button::kClose)) {
                CloseLocked();
            }
        }
    } else if (!swiped_ && !moved && duration <= kTapMaxMs) {
        if (second_tap_) {
            second_tap_ = false;
            last_tap_ms_ = 0;
            action = Action::kDoubleTap;
        } else {
            last_tap_ms_ = now_ms;
            last_tap_x_ = x;
            last_tap_y_ = y;
            action = Action::kSingleTap;
        }
    }

    drag_target_ = DragTarget::kNone;
    drag_armed_ = false;
    Unlock();
    return action;
}

}  // namespace cyberai
