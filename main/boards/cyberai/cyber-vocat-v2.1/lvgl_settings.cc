#include "lvgl_settings.h"

#include <cstdio>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>

#include "application.h"
#include "board.h"
#include "settings.h"
#include "widget/gfx_font_lvgl.h"

namespace cyberai {

namespace {

const char* TAG = "LvglSettings";

// Draw buffer: 15 rows is 10.8 KB per buffer. The panel goes through cyberai::DmaPanel, whose
// staging buffer is 11520 bytes, and it rejects any transfer larger than that — 16 rows would
// sit exactly on the limit, 15 leaves a margin.
constexpr int kDrawBufLines = 15;
constexpr int kTaskStack = 8 * 1024;
constexpr int kTaskPriority = 2;
constexpr int64_t kIdleCloseMs = 10000;
constexpr int kBrightnessMin = 5;  // Never dim the screen to black.

constexpr uint32_t kColorBg = 0x0E1116;
constexpr uint32_t kColorText = 0xF2F4F8;
constexpr uint32_t kColorDim = 0x9AA4B8;
constexpr uint32_t kColorTrack = 0x2C3240;
constexpr uint32_t kColorAccentBright = 0xFFC447;
constexpr uint32_t kColorAccentVolume = 0x4FC3F7;
constexpr uint32_t kColorButton = 0x2A3140;
constexpr uint32_t kColorAecOn = 0x2E7D32;
constexpr uint32_t kColorAecOff = 0x5A3038;

constexpr int kScreenOffMaxMinutes = 30;
constexpr int kScreenOffDefaultMinutes = 10;

}  // namespace

LvglSettings::LvglSettings(esp_lcd_panel_handle_t panel, emote::EmoteDisplay* display,
                           Backlight* backlight, int width, int height)
    : panel_(panel), display_(display), backlight_(backlight), width_(width), height_(height) {
    // Live state (volume, brightness) is read in Show(): the board constructor runs this before
    // the application and the audio codec exist, so touching them here would block board init.
    aec_enabled_ = Application::GetInstance().GetAecMode() != kAecOff;
    // The screen-off timeout has to be known before the page is ever opened, so it is read here
    // (plain NVS, no audio codec involved).
    Settings display_settings("display", true);
    int minutes = display_settings.GetInt("screen_off_min", kScreenOffDefaultMinutes);
    if (minutes < 0) {
        minutes = 0;
    } else if (minutes > kScreenOffMaxMinutes) {
        minutes = kScreenOffMaxMinutes;
    }
    screen_off_minutes_.store(minutes);
    ESP_LOGI(TAG, "Created (panel %p, AEC %s, screen off %d min)", panel_,
             aec_enabled_ ? "on" : "off", minutes);
}

LvglSettings::~LvglSettings() {
    Hide();
    task_running_.store(false);
    for (int i = 0; i < 40 && task_ != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (idle_timer_ != nullptr) {
        esp_timer_stop(idle_timer_);
        esp_timer_delete(idle_timer_);
        idle_timer_ = nullptr;
    }
    if (screen_ != nullptr) {
        lv_obj_delete(screen_);
        screen_ = nullptr;
    }
    if (lv_indev_ != nullptr) {
        lv_indev_delete(lv_indev_);
        lv_indev_ = nullptr;
    }
    if (lv_display_ != nullptr) {
        lv_display_delete(lv_display_);
        lv_display_ = nullptr;
    }
    if (flush_done_ != nullptr) {
        vSemaphoreDelete(flush_done_);
        flush_done_ = nullptr;
    }
    if (draw_buf_1_ != nullptr) {
        heap_caps_free(draw_buf_1_);
        draw_buf_1_ = nullptr;
    }
    if (draw_buf_2_ != nullptr) {
        heap_caps_free(draw_buf_2_);
        draw_buf_2_ = nullptr;
    }
}

bool LvglSettings::Initialize() {
    if (panel_ == nullptr || width_ <= 0 || height_ <= 0) {
        ESP_LOGE(TAG, "No panel or geometry, settings page unavailable");
        return false;
    }

    if (!lv_is_initialized()) {
        lv_init();
        lv_tick_set_cb([]() -> uint32_t {
            return static_cast<uint32_t>(esp_timer_get_time() / 1000);
        });
    }
    ESP_LOGI(TAG, "LVGL initialised");

    const size_t buf_bytes =
        static_cast<size_t>(width_) * kDrawBufLines * sizeof(uint16_t);
    draw_buf_1_ = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    draw_buf_2_ = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (draw_buf_1_ == nullptr || draw_buf_2_ == nullptr) {
        ESP_LOGW(TAG, "No internal DMA buffer for LVGL, falling back to PSRAM");
        if (draw_buf_1_ == nullptr) {
            draw_buf_1_ = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
        }
        if (draw_buf_2_ == nullptr) {
            draw_buf_2_ = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
        }
    }
    if (draw_buf_1_ == nullptr || draw_buf_2_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate %u byte draw buffers", static_cast<unsigned>(buf_bytes));
        return false;
    }
    ESP_LOGI(TAG, "Draw buffers: 2 x %u bytes", static_cast<unsigned>(buf_bytes));

    lv_display_ = lv_display_create(width_, height_);
    if (lv_display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create LVGL display");
        return false;
    }
    lv_display_set_flush_cb(lv_display_, &LvglSettings::FlushCallback);
    // This panel wants RGB565 bytes in swapped order - the emote renderer swaps them too
    // (gfx_color_to_native_u16(..., swap)). Without this the whole page comes out in the wrong
    // hues: the dark blue background renders as maroon and green buttons as yellow.
    lv_display_set_color_format(lv_display_, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_buffers(lv_display_, draw_buf_1_, draw_buf_2_, static_cast<uint32_t>(buf_bytes),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(lv_display_, this);

    lv_indev_ = lv_indev_create();
    if (lv_indev_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create LVGL input device");
        return false;
    }
    lv_indev_set_type(lv_indev_, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(lv_indev_, &LvglSettings::ReadCallback);
    lv_indev_set_display(lv_indev_, lv_display_);
    lv_indev_set_user_data(lv_indev_, this);

    flush_done_ = xSemaphoreCreateBinary();
    if (flush_done_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create flush semaphore");
        return false;
    }

    ESP_LOGI(TAG, "Display and input device ready");

    const esp_timer_create_args_t timer_args = {
        .callback = &LvglSettings::IdleTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "lv_page_idle",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&timer_args, &idle_timer_) != ESP_OK) {
        ESP_LOGW(TAG, "Idle timer unavailable, page will not auto-close");
        idle_timer_ = nullptr;
    }

    ready_ = true;
    ESP_LOGI(TAG, "Settings page ready (brightness %d%%, volume %d%%, AEC %s)", brightness_,
             volume_, aec_enabled_ ? "on" : "off");
    return true;
}

void LvglSettings::LoadFonts() {
    if (display_ == nullptr || font_main_ != nullptr) {
        return;
    }
    // The asset pack stores files under their own names, so the CJK pixel font this board
    // already ships is looked up by the exact name emote itself resolves: see
    // emote_load.c emote_load_fonts(), whose log reads "Foundfont: font_puhui_common_20_4.bin".
    // Loading is deferred to Show() because the asset partition is only mounted while the
    // application starts, after the board constructor has run.
    static const char* kFontNames[] = {
        "font_puhui_common_20_4.bin",
        "font_puhui_common_20_4",
        "font/font_puhui_common_20_4.bin",
    };
    const uint8_t* data = nullptr;
    size_t size = 0;
    for (const char* name : kFontNames) {
        if (display_->GetAssetData(name, data, size) && data != nullptr && size > 0) {
            ESP_LOGI(TAG, "Parsing %u byte CJK font '%s'", static_cast<unsigned>(size), name);
            font_main_ = gfx_font_lv_load_from_binary(const_cast<uint8_t*>(data));
            break;
        }
    }
    if (font_main_ == nullptr) {
        ESP_LOGE(TAG, "CJK font unavailable, page text will be garbled");
    }
}

bool LvglSettings::Show() {
    if (!ready_ || open_.load()) {
        return false;
    }

    // Sample the live state now rather than in the constructor: the audio codec only exists
    // once the application has started.
    if (backlight_ != nullptr) {
        brightness_ = backlight_->brightness();
        if (brightness_ < kBrightnessMin) {
            brightness_ = kBrightnessMin;
        }
    }
    if (auto* codec = Board::GetInstance().GetAudioCodec()) {
        volume_ = codec->output_volume();
    }
    aec_enabled_ = Application::GetInstance().GetAecMode() != kAecOff;

    // Take the panel from emote first: from here on it keeps running but draws nothing.
    emote::SetFlushDoneHook(&LvglSettings::FlushDoneHook, this);
    emote::SetPanelSuspended(true);

    LoadFonts();
    BuildUi();
    if (screen_ == nullptr) {
        emote::SetPanelSuspended(false);
        emote::SetFlushDoneHook(nullptr, nullptr);
        return false;
    }

    open_.store(true);
    task_running_.store(true);
    if (xTaskCreate(&LvglSettings::TaskEntry, "lvgl_page", kTaskStack, this, kTaskPriority,
                    &task_) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start LVGL task");
        task_ = nullptr;
        task_running_.store(false);
        open_.store(false);
        lv_obj_delete(screen_);
        screen_ = nullptr;
        emote::SetPanelSuspended(false);
        emote::SetFlushDoneHook(nullptr, nullptr);
        return false;
    }

    RestartIdleTimer();
    // Give the task a moment to paint the first frame, so the caller returns with the page
    // already on screen instead of a black panel.
    vTaskDelay(pdMS_TO_TICKS(80));
    ESP_LOGI(TAG, "Settings page shown");
    return true;
}

void LvglSettings::Hide() {
    if (!open_.load()) {
        return;
    }
    open_.store(false);
    pressed_.store(false);
    if (idle_timer_ != nullptr) {
        esp_timer_stop(idle_timer_);
    }

    task_running_.store(false);
    for (int i = 0; i < 40 && task_ != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    // The LVGL task is gone, so the widgets can be destroyed from this context.
    if (screen_ != nullptr) {
        lv_obj_delete(screen_);
        screen_ = nullptr;
    }

    // Hand the panel back and let emote repaint everything over what the page left behind.
    emote::SetPanelSuspended(false);
    emote::SetFlushDoneHook(nullptr, nullptr);
    if (display_ != nullptr) {
        display_->RefreshAll();
    }
    ESP_LOGI(TAG, "Settings page hidden");
}

bool LvglSettings::ConsumeCloseRequest() {
    return close_requested_.exchange(false);
}

bool LvglSettings::ConsumeWifiRequest() {
    return wifi_requested_.exchange(false);
}

bool LvglSettings::HandlePendingCommits() {
    bool screen_off_changed = false;
    if (volume_commit_requested_.exchange(false)) {
        auto* codec = Board::GetInstance().GetAudioCodec();
        if (codec != nullptr) {
            codec->SetOutputVolume(volume_);
        }
        if (backlight_ != nullptr) {
            // Persist the brightness the user just settled on, the same way the slider does
            // while it is being dragged.
            backlight_->SetBrightness(static_cast<uint8_t>(brightness_), true);
        }
        ESP_LOGI(TAG, "Committed volume %d%%, brightness %d%%", volume_, brightness_);
    }
    if (screen_off_commit_requested_.exchange(false)) {
        int minutes = screen_off_pending_minutes_.load();
        Settings display_settings("display", true);
        // NVS keys are capped at 15 bytes: "screen_off_minutes" aborted the device with
        // ESP_ERR_NVS_KEY_TOO_LONG, so the key is the abbreviated form.
        display_settings.SetInt("screen_off_min", minutes);
        screen_off_changed = true;
        ESP_LOGI(TAG, "Screen-off timeout committed: %d min", minutes);
    }
    return screen_off_changed;
}

void LvglSettings::FeedTouch(bool pressed, int x, int y) {
    touch_x_.store(x);
    touch_y_.store(y);
    pressed_.store(pressed);
    if (pressed) {
        RestartIdleTimer();
    }
}

void LvglSettings::RestartIdleTimer() {
    if (idle_timer_ == nullptr || !open_.load()) {
        return;
    }
    esp_timer_stop(idle_timer_);
    esp_timer_start_once(idle_timer_, static_cast<uint64_t>(kIdleCloseMs) * 1000ULL);
}

void LvglSettings::IdleTimerCallback(void* arg) {
    auto* self = static_cast<LvglSettings*>(arg);
    if (self != nullptr && self->open_.load()) {
        self->close_requested_.store(true);
        ESP_LOGI(TAG, "Idle timeout, page asked to close itself");
    }
}

void LvglSettings::TaskEntry(void* arg) {
    auto* self = static_cast<LvglSettings*>(arg);
    if (self != nullptr) {
        self->TaskLoop();
        self->task_ = nullptr;
    }
    vTaskDelete(nullptr);
}

void LvglSettings::TaskLoop() {
    while (task_running_.load()) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void LvglSettings::FlushCallback(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    auto* self = static_cast<LvglSettings*>(lv_display_get_user_data(disp));
    if (self == nullptr || self->panel_ == nullptr) {
        lv_display_flush_ready(disp);
        return;
    }

    esp_err_t err = esp_lcd_panel_draw_bitmap(self->panel_, area->x1, area->y1, area->x2 + 1,
                                              area->y2 + 1, px_map);
    if (err == ESP_OK && self->flush_done_ != nullptr) {
        // Drop a stale completion left by a previous timed-out transfer before waiting.
        while (xSemaphoreTake(self->flush_done_, 0) == pdTRUE) {
        }
        if (xSemaphoreTake(self->flush_done_, pdMS_TO_TICKS(200)) != pdTRUE &&
            !self->flush_timeout_logged_) {
            ESP_LOGW(TAG, "Panel transfer did not complete in time");
            self->flush_timeout_logged_ = true;
        }
    }
    lv_display_flush_ready(disp);
}

void LvglSettings::FlushDoneHook(void* ctx) {
    auto* self = static_cast<LvglSettings*>(ctx);
    if (self == nullptr || self->flush_done_ == nullptr) {
        return;
    }
    BaseType_t higher_priority_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(self->flush_done_, &higher_priority_task_woken);
    if (higher_priority_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

void LvglSettings::ReadCallback(lv_indev_t* indev, lv_indev_data_t* data) {
    auto* self = static_cast<LvglSettings*>(lv_indev_get_user_data(indev));
    if (self == nullptr) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    data->point.x = static_cast<int32_t>(self->touch_x_.load());
    data->point.y = static_cast<int32_t>(self->touch_y_.load());
    data->state = self->pressed_.load() ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void LvglSettings::BuildUi() {
    screen_ = lv_obj_create(nullptr);
    if (screen_ == nullptr) {
        return;
    }
    lv_obj_set_style_bg_color(screen_, lv_color_hex(kColorBg), 0);
    lv_obj_set_style_bg_opa(screen_, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen_, lv_color_hex(kColorText), 0);
    lv_obj_set_style_text_font(screen_, font_main_ != nullptr ? font_main_ : LV_FONT_DEFAULT, 0);
    lv_obj_set_style_pad_all(screen_, 0, 0);
    lv_obj_remove_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(screen_);
    lv_label_set_text(title, "设置");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);

    // Brightness
    lv_obj_t* brightness_caption = lv_label_create(screen_);
    lv_label_set_text(brightness_caption, "亮度");
    lv_obj_set_pos(brightness_caption, 58, 72);

    brightness_slider_ = lv_slider_create(screen_);
    lv_obj_set_pos(brightness_slider_, 118, 75);
    lv_obj_set_size(brightness_slider_, 124, 20);
    lv_slider_set_range(brightness_slider_, kBrightnessMin, 100);
    lv_slider_set_value(brightness_slider_, brightness_, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(brightness_slider_, lv_color_hex(kColorTrack), LV_PART_MAIN);
    lv_obj_set_style_bg_color(brightness_slider_, lv_color_hex(kColorAccentBright),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(brightness_slider_, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(brightness_slider_, 2, LV_PART_KNOB);
    lv_obj_add_event_cb(brightness_slider_, &LvglSettings::SliderEventCallback,
                        LV_EVENT_VALUE_CHANGED, this);

    brightness_value_ = lv_label_create(screen_);
    lv_obj_set_pos(brightness_value_, 246, 72);
    lv_obj_set_width(brightness_value_, 52);
    lv_obj_set_style_text_align(brightness_value_, LV_TEXT_ALIGN_RIGHT, 0);

    // Volume
    lv_obj_t* volume_caption = lv_label_create(screen_);
    lv_label_set_text(volume_caption, "音量");
    lv_obj_set_pos(volume_caption, 58, 124);

    volume_slider_ = lv_slider_create(screen_);
    lv_obj_set_pos(volume_slider_, 118, 127);
    lv_obj_set_size(volume_slider_, 124, 20);
    lv_slider_set_range(volume_slider_, 0, 100);
    lv_slider_set_value(volume_slider_, volume_, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(volume_slider_, lv_color_hex(kColorTrack), LV_PART_MAIN);
    lv_obj_set_style_bg_color(volume_slider_, lv_color_hex(kColorAccentVolume),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(volume_slider_, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(volume_slider_, 2, LV_PART_KNOB);
    lv_obj_add_event_cb(volume_slider_, &LvglSettings::SliderEventCallback,
                        LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(volume_slider_, &LvglSettings::SliderEventCallback, LV_EVENT_RELEASED,
                        this);

    volume_value_ = lv_label_create(screen_);
    lv_obj_set_pos(volume_value_, 246, 124);
    lv_obj_set_width(volume_value_, 52);
    lv_obj_set_style_text_align(volume_value_, LV_TEXT_ALIGN_RIGHT, 0);

    // Screen-off timeout: 0 means the screen never turns itself off.
    lv_obj_t* screen_off_caption = lv_label_create(screen_);
    lv_label_set_text(screen_off_caption, "息屏");
    lv_obj_set_pos(screen_off_caption, 58, 176);

    screen_off_slider_ = lv_slider_create(screen_);
    lv_obj_set_pos(screen_off_slider_, 118, 179);
    lv_obj_set_size(screen_off_slider_, 124, 20);
    lv_slider_set_range(screen_off_slider_, 0, kScreenOffMaxMinutes);
    lv_slider_set_value(screen_off_slider_, screen_off_minutes_.load(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(screen_off_slider_, lv_color_hex(kColorTrack), LV_PART_MAIN);
    lv_obj_set_style_bg_color(screen_off_slider_, lv_color_hex(kColorDim), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(screen_off_slider_, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(screen_off_slider_, 2, LV_PART_KNOB);
    lv_obj_add_event_cb(screen_off_slider_, &LvglSettings::SliderEventCallback,
                        LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(screen_off_slider_, &LvglSettings::SliderEventCallback, LV_EVENT_RELEASED,
                        this);

    screen_off_value_ = lv_label_create(screen_);
    lv_obj_set_pos(screen_off_value_, 246, 176);
    lv_obj_set_width(screen_off_value_, 52);
    lv_obj_set_style_text_align(screen_off_value_, LV_TEXT_ALIGN_RIGHT, 0);

    // Network line
    info_label_ = lv_label_create(screen_);
    lv_obj_set_pos(info_label_, 44, 208);
    lv_obj_set_width(info_label_, 272);
    lv_obj_set_style_text_align(info_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(info_label_, lv_color_hex(kColorDim), 0);

    // AEC
    aec_button_ = lv_button_create(screen_);
    lv_obj_set_pos(aec_button_, 80, 282);
    lv_obj_set_size(aec_button_, 96, 36);
    lv_obj_set_style_radius(aec_button_, 14, 0);
    lv_obj_set_style_shadow_width(aec_button_, 0, 0);
    aec_label_ = lv_label_create(aec_button_);
    lv_obj_center(aec_label_);
    lv_obj_add_event_cb(aec_button_, &LvglSettings::ButtonEventCallback, LV_EVENT_CLICKED, this);

    // Wi-Fi configuration
    wifi_button_ = lv_button_create(screen_);
    lv_obj_set_pos(wifi_button_, 184, 282);
    lv_obj_set_size(wifi_button_, 96, 36);
    lv_obj_set_style_radius(wifi_button_, 14, 0);
    lv_obj_set_style_shadow_width(wifi_button_, 0, 0);
    lv_obj_set_style_bg_color(wifi_button_, lv_color_hex(kColorButton), 0);
    lv_obj_t* wifi_text = lv_label_create(wifi_button_);
    lv_label_set_text(wifi_text, "配网");
    lv_obj_center(wifi_text);
    lv_obj_add_event_cb(wifi_button_, &LvglSettings::ButtonEventCallback, LV_EVENT_CLICKED, this);

    RenderBrightness();
    RenderVolume();
    RenderScreenOff();
    RenderAec();
    RefreshInfo();

    lv_screen_load(screen_);
}

void LvglSettings::SliderEventCallback(lv_event_t* event) {
    auto* self = static_cast<LvglSettings*>(lv_event_get_user_data(event));
    auto* target = static_cast<lv_obj_t*>(lv_event_get_target(event));
    if (self == nullptr || target == nullptr) {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(event);
    self->RestartIdleTimer();

    if (target == self->brightness_slider_) {
        self->brightness_ = static_cast<int>(lv_slider_get_value(target));
        self->RenderBrightness();
        self->ApplyBrightness(self->brightness_);
    } else if (target == self->volume_slider_) {
        self->volume_ = static_cast<int>(lv_slider_get_value(target));
        self->RenderVolume();
        if (code == LV_EVENT_RELEASED) {
            self->CommitVolume();
        }
    } else if (target == self->screen_off_slider_) {
        self->screen_off_minutes_.store(static_cast<int>(lv_slider_get_value(target)));
        self->RenderScreenOff();
        if (code == LV_EVENT_RELEASED) {
            self->CommitScreenOff(self->screen_off_minutes_.load());
        }
    }
}

void LvglSettings::ButtonEventCallback(lv_event_t* event) {
    auto* self = static_cast<LvglSettings*>(lv_event_get_user_data(event));
    auto* target = static_cast<lv_obj_t*>(lv_event_get_target(event));
    if (self == nullptr || target == nullptr) {
        return;
    }
    self->RestartIdleTimer();

    if (target == self->aec_button_) {
        self->ToggleAec();
    } else if (target == self->wifi_button_) {
        self->wifi_requested_.store(true);
    }
}

void LvglSettings::RenderBrightness() {
    if (brightness_value_ != nullptr) {
        lv_label_set_text_fmt(brightness_value_, "%d%%", brightness_);
    }
}

void LvglSettings::RenderVolume() {
    if (volume_value_ != nullptr) {
        lv_label_set_text_fmt(volume_value_, "%d%%", volume_);
    }
}

void LvglSettings::RenderScreenOff() {
    if (screen_off_value_ == nullptr) {
        return;
    }
    const int minutes = screen_off_minutes_.load();
    if (minutes <= 0) {
        lv_label_set_text(screen_off_value_, "关闭");
    } else {
        lv_label_set_text_fmt(screen_off_value_, "%d 分", minutes);
    }
}

void LvglSettings::RenderAec() {
    if (aec_label_ != nullptr) {
        lv_label_set_text(aec_label_, aec_enabled_ ? "AEC 开" : "AEC 关");
    }
    if (aec_button_ != nullptr) {
        lv_obj_set_style_bg_color(aec_button_,
                                  lv_color_hex(aec_enabled_ ? kColorAecOn : kColorAecOff), 0);
    }
}

void LvglSettings::RefreshInfo() {
    if (info_label_ == nullptr) {
        return;
    }
    char text[128] = "未连接";
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        char ip[32] = "-";
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif != nullptr) {
            esp_netif_ip_info_t ip_info = {};
            if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ip_info.ip));
            }
        }
        snprintf(text, sizeof(text), "%s  %ddBm\n%s", reinterpret_cast<const char*>(ap.ssid),
                 ap.rssi, ip);
    }
    lv_label_set_text(info_label_, text);
}

void LvglSettings::ApplyBrightness(int value) {
    if (backlight_ == nullptr) {
        return;
    }
    if (value < kBrightnessMin) {
        value = kBrightnessMin;
    }
    backlight_->SetBrightness(static_cast<uint8_t>(value), false);
}

void LvglSettings::CommitVolume() {
    // Only raise the request: the owner task performs the NVS write.
    volume_commit_requested_.store(true);
}

void LvglSettings::CommitScreenOff(int minutes) {
    if (minutes < 0) {
        minutes = 0;
    } else if (minutes > kScreenOffMaxMinutes) {
        minutes = kScreenOffMaxMinutes;
    }
    screen_off_minutes_.store(minutes);
    // Only raise the request: the owner task performs the NVS write.
    screen_off_pending_minutes_.store(minutes);
    screen_off_commit_requested_.store(true);
}

void LvglSettings::ToggleAec() {
    aec_enabled_ = !aec_enabled_;
    Application::GetInstance().SetAecMode(aec_enabled_ ? kAecOnDeviceSide : kAecOff);
    Settings settings("aec", true);
    settings.SetInt("aec_mode", aec_enabled_ ? 1 : 0);
    RenderAec();
    ESP_LOGI(TAG, "AEC %s", aec_enabled_ ? "on" : "off");
}

}  // namespace cyberai
