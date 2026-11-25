#pragma once

#include <atomic>
#include <cstdint>

#include <esp_lcd_panel_io.h>
#include <esp_lcd_types.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lvgl.h>

#include "backlight.h"
#include "display/emote_display.h"

namespace cyberai {

// A full page of the device settings, drawn with LVGL on top of the emote face.
//
// The board only has one panel and one flush callback, both owned by emote. While the page
// is open, emote keeps running but its frames are dropped (EmoteDisplay::SetPanelSuspended),
// and LVGL takes the panel over. Closing hands the panel back and forces a full repaint, so
// no fragments of the page survive on screen.
//
// Threading contract:
//   * Show()/Hide() must be called from the same owner (the board touch task).
//   * The LVGL task only reads the atomic touch state and never touches I2C.
//   * Closing the page and entering Wi-Fi config mode are reported back as request flags;
//     the LVGL task never tears down its own widgets.
class LvglSettings {
public:
    LvglSettings(esp_lcd_panel_handle_t panel, emote::EmoteDisplay* display, Backlight* backlight,
                 int width, int height);
    ~LvglSettings();

    bool Initialize();
    bool Show();
    void Hide();
    bool IsOpen() const { return open_.load(); }

    // Minutes of inactivity before the board blanks the screen; 0 means never.
    int ScreenOffMinutes() const { return screen_off_minutes_.load(); }

    // Touch samples pushed in by the board touch task (raw panel coordinates).
    void FeedTouch(bool pressed, int x, int y);

    // Decisions the page wants the owner to carry out.
    bool ConsumeCloseRequest();
    bool ConsumeWifiRequest();
    // Performs the NVS writes the page asked for. Called by the owner (the board
    // touch task): the LVGL task stack is too small for a flash write. Returns true
    // when the screen-off timeout changed, so the owner re-arms its idle timer.
    bool HandlePendingCommits();

private:
    static void TaskEntry(void* arg);
    static void FlushCallback(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map);
    static void FlushDoneHook(void* ctx);
    static void ReadCallback(lv_indev_t* indev, lv_indev_data_t* data);
    static void SliderEventCallback(lv_event_t* event);
    static void ButtonEventCallback(lv_event_t* event);
    static void IdleTimerCallback(void* arg);

    void TaskLoop();
    void BuildUi();
    void LoadFonts();
    void RenderBrightness();
    void RenderVolume();
    void RenderScreenOff();
    void RenderAec();
    void RefreshInfo();
    void ApplyBrightness(int value);
    void CommitVolume();
    void CommitScreenOff(int minutes);
    void ToggleAec();
    void RestartIdleTimer();

    esp_lcd_panel_handle_t panel_ = nullptr;
    emote::EmoteDisplay* display_ = nullptr;
    Backlight* backlight_ = nullptr;
    int width_ = 0;
    int height_ = 0;

    lv_display_t* lv_display_ = nullptr;
    lv_indev_t* lv_indev_ = nullptr;
    lv_obj_t* screen_ = nullptr;
    const lv_font_t* font_main_ = nullptr;
    void* draw_buf_1_ = nullptr;
    void* draw_buf_2_ = nullptr;

    lv_obj_t* brightness_slider_ = nullptr;
    lv_obj_t* brightness_value_ = nullptr;
    lv_obj_t* volume_slider_ = nullptr;
    lv_obj_t* volume_value_ = nullptr;
    lv_obj_t* screen_off_slider_ = nullptr;
    lv_obj_t* screen_off_value_ = nullptr;
    lv_obj_t* aec_button_ = nullptr;
    lv_obj_t* aec_label_ = nullptr;
    lv_obj_t* wifi_button_ = nullptr;
    lv_obj_t* info_label_ = nullptr;

    SemaphoreHandle_t flush_done_ = nullptr;
    TaskHandle_t task_ = nullptr;
    esp_timer_handle_t idle_timer_ = nullptr;

    std::atomic<bool> open_{false};
    std::atomic<bool> task_running_{false};
    std::atomic<bool> pressed_{false};
    std::atomic<bool> close_requested_{false};
    std::atomic<bool> wifi_requested_{false};
    std::atomic<bool> screen_off_commit_requested_{false};
    std::atomic<bool> volume_commit_requested_{false};
    std::atomic<int> screen_off_pending_minutes_{10};
    std::atomic<int> touch_x_{0};
    std::atomic<int> touch_y_{0};

    bool ready_ = false;
    bool flush_timeout_logged_ = false;
    int brightness_ = 70;
    int volume_ = 50;
    bool aec_enabled_ = true;
    std::atomic<int> screen_off_minutes_{10};
};

}  // namespace cyberai
