#pragma once

#include "display/lcd_display.h"

#include <cstdint>

// Scheme A "bean eyes" from docs/kodo-face-design (320x240): the whole face is
// two rounded-rectangle eyes that morph between eight moods.
class KodoFaceDisplay : public SpiLcdDisplay {
public:
    KodoFaceDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                    int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                    bool swap_xy);

    void SetupUI() override;
    void SetStatus(const char* status) override;
    bool SetEmotion(const char* emotion) override;
    void SetPowerSaveMode(bool on) override;
    void SetTheme(Theme* theme) override;

private:
    enum class Face : uint8_t {
        kIdle,
        kHappy,
        kListen,
        kSpeak,
        kThink,
        kSurprise,
        kSleep,
        kSad,
    };

    struct Eye {
        int16_t cx;
        int16_t cy;
        int16_t w;
        int16_t h;
        int16_t max_radius;
    };

    // Everything that moves between moods. Opacities are 0..255.
    struct Pose {
        Eye eye[2];
        uint8_t eye_opa;
        uint8_t smile_opa;
        uint8_t lid_opa;
        uint8_t dots_opa;
        uint8_t zz_opa;
    };

    void ApplyPalette();
    void BuildFace();
    void Show(Face face);
    void ArmWakeAck(Face next);
    void LeaveSleep();
    bool ClassifyStatus(const char* status, Face* face) const;
    bool MapEmotion(const char* emotion, Face* face) const;
    void OnTick();
    void StartBlink();
    void Apply();
    void Render(const Pose& pose);

    static Pose Target(Face face);
    static bool Blinks(Face face);
    static void TickThunk(lv_timer_t* timer);
    static void TransitionExec(void* var, int32_t value);
    static void BlinkExec(void* var, int32_t value);
    static void LidDrawThunk(lv_event_t* e);

    bool ready_ = false;
    bool sleeping_ = false;
    bool acking_ = false;
    uint32_t ack_start_ms_ = 0;
    Face mode_ = Face::kIdle;
    Face current_ = Face::kIdle;

    lv_obj_t* canvas_ = nullptr;
    lv_obj_t* eye_[2] = {};
    lv_obj_t* smile_[2] = {};
    lv_obj_t* dot_[3] = {};
    lv_obj_t* zz_[2] = {};
    lv_timer_t* timer_ = nullptr;

    Pose from_ = {};
    Pose to_ = {};
    Pose shown_ = {};
    Pose rendered_ = {};
    bool rendered_valid_ = false;
    int32_t progress_ = 0;
    int32_t blink_ = 0;
    bool blinking_ = false;
    uint32_t next_blink_ms_ = 0;
    uint32_t speak_start_ms_ = 0;

    lv_point_precise_t smile_pts_[2][13] = {};
    lv_point_precise_t zz_pts_[2][4] = {};
};
