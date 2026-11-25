#pragma once

#include <esp_timer.h>

#include <cstdint>

#include "expression_emote.h"

class Backlight;

namespace cyberai {

// Pull-down settings drawer for the 360x360 round panel.
//
// Everything visible here is an emote/gfx label object created at runtime, so the
// drawer needs neither an LVGL page nor an asset-partition rebuild. The board keeps
// ownership of the touch driver and feeds raw CST816S coordinates into this class;
// all hit testing happens here on absolute panel coordinates.
class TouchPanel {
public:
    // Result of a completed press/move/release sequence. The board decides what a
    // tap means for the device state (single tap stays intentionally inert while the
    // panel is closed, except for the boot-time Wi-Fi config shortcut).
    enum class Action {
        kNone,
        kSingleTap,
        kDoubleTap,
        kWifiConfig,
    };

    TouchPanel(emote_handle_t emote_handle, Backlight* backlight);
    ~TouchPanel();

    TouchPanel(const TouchPanel&) = delete;
    TouchPanel& operator=(const TouchPanel&) = delete;

    // Creates every object hidden. Returns false when the drawer cannot be built, in
    // which case the caller keeps the plain touch behaviour.
    bool Initialize();

    bool IsOpen() const { return open_; }

    void OnPress(int x, int y, int64_t now_ms);
    void OnMove(int x, int y, int64_t now_ms);
    Action OnRelease(int x, int y, int64_t now_ms);

private:
    enum class DragTarget { kNone, kBrightness, kVolume };
    enum class Button { kAec, kWifi, kClose };

    // Named emote objects that make up the home scene, eye animation included.
    static constexpr int kSceneObjectCount = 10;
    struct SceneRef {
        gfx_obj_t* obj;
        bool hidden_by_page;
    };

    void Lock() const;
    void Unlock() const;

    bool CreateObjects();
    void SetVisibleLocked(bool visible);
    void RenderAllLocked();
    void RenderBrightnessLocked();
    void RenderVolumeLocked();
    void RenderAecLocked();
    void RefreshSourcesLocked();
    void RefreshNetworkLocked();

    void ApplyDragLocked(int x);
    void CommitDragLocked();
    void ToggleAecLocked();

    void OpenLocked();
    void CloseLocked();
    void RestartIdleTimerLocked();
    bool HitButton(int x, int y, Button button) const;

    // The eye animation and the idle hints are separate emote objects, and emote
    // re-shows them whenever the device state or emotion changes - so the page keeps
    // re-asserting "scene hidden" while it is open, not just once on open.
    void HideSceneLocked();
    void RestoreSceneLocked();
    void LogGeometryLocked() const;
    static void GuardTimerCallback(void* arg);

    static void IdleTimerCallback(void* arg);

    emote_handle_t emote_ = nullptr;
    Backlight* backlight_ = nullptr;
    esp_timer_handle_t idle_timer_ = nullptr;
    esp_timer_handle_t guard_timer_ = nullptr;
    bool ready_ = false;
    bool open_ = false;

    gfx_obj_t* bg_ = nullptr;
    gfx_obj_t* title_ = nullptr;
    gfx_obj_t* brightness_caption_ = nullptr;
    gfx_obj_t* brightness_track_ = nullptr;
    gfx_obj_t* brightness_fill_ = nullptr;
    gfx_obj_t* brightness_knob_ = nullptr;
    gfx_obj_t* volume_caption_ = nullptr;
    gfx_obj_t* volume_track_ = nullptr;
    gfx_obj_t* volume_fill_ = nullptr;
    gfx_obj_t* volume_knob_ = nullptr;
    gfx_obj_t* wifi_label_ = nullptr;
    gfx_obj_t* ip_label_ = nullptr;
    gfx_obj_t* aec_button_ = nullptr;
    gfx_obj_t* wifi_button_ = nullptr;
    gfx_obj_t* close_button_ = nullptr;

    int brightness_ = 70;
    int volume_ = 70;
    bool aec_enabled_ = false;

    SceneRef scene_[kSceneObjectCount] = {};

    int press_x_ = 0;
    int press_y_ = 0;
    int64_t press_ms_ = 0;
    int last_x_ = 0;
    int last_y_ = 0;
    DragTarget drag_target_ = DragTarget::kNone;
    bool drag_armed_ = false;
    bool swiped_ = false;
    bool second_tap_ = false;
    int64_t last_tap_ms_ = 0;
    int last_tap_x_ = 0;
    int last_tap_y_ = 0;
};

}  // namespace cyberai
