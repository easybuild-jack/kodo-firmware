#pragma once

#include "display/lcd_display.h"

#include <cstdint>

// Vector face from docs/xiaorui-face-design (320x240). Eyes, mouth and
// decorations change per expression; the chassis stays put.
class XiaoruiFaceDisplay : public SpiLcdDisplay {
public:
    XiaoruiFaceDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                       int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                       bool swap_xy);

    void SetupUI() override;
    void SetStatus(const char* status) override;
    bool SetEmotion(const char* emotion) override;
    void SetPowerSaveMode(bool on) override;

private:
    enum class Face : uint8_t {
        kIdle,
        kListen,
        kSpeak,
        kThink,
        kHappy,
        kWink,
        kSurprise,
        kConfuse,
        kSad,
        kSleep,
        kLook,
        kLove,
        kShy,
        kOops,
    };

    void BuildChassis();
    void Show(Face face);
    void ArmWakeAck(Face next);
    void LeaveSleep();
    Face FaceForMode() const;
    bool ClassifyStatus(const char* status, Face* face) const;
    bool MapEmotion(const char* emotion, Face* face) const;
    void OnTick();

    lv_obj_t* Dot(lv_obj_t* parent, int cx, int cy, int r, lv_color_t fill, lv_opa_t opa);
    lv_obj_t* Oval(lv_obj_t* parent, int cx, int cy, int rx, int ry, lv_color_t fill,
                   lv_opa_t fill_opa, lv_color_t stroke, int stroke_w);
    lv_obj_t* Chip(lv_obj_t* parent, int x, int y, int w, int h, int radius, lv_color_t fill,
                   lv_color_t stroke, int stroke_w);
    const lv_point_precise_t* AddQuad(int x0, int y0, int cx, int cy, int x1, int y1,
                                      uint32_t* count);
    const lv_point_precise_t* AddSeg(int x0, int y0, int x1, int y1, uint32_t* count);
    lv_obj_t* Stroke(const lv_point_precise_t* pts, uint32_t count, int width, lv_color_t color);
    void OpenEye(int cx, int cy, int rx, int ry, int pupil_r, int pupil_dx, int pupil_dy,
                 bool left);
    void Crescent(int x0, int y0, int cx, int cy, int x1, int y1, int width);
    void Blush(lv_opa_t opa);
    void ClearFeatures();

    static void TickThunk(lv_timer_t* timer);

    bool ready_ = false;
    bool sleeping_ = false;
    bool acking_ = false;
    int ack_ticks_ = 0;
    Face mode_ = Face::kIdle;
    Face current_ = Face::kIdle;

    lv_obj_t* canvas_ = nullptr;
    lv_obj_t* features_ = nullptr;
    lv_obj_t* antenna_ball_ = nullptr;
    lv_obj_t* chin_mid_ = nullptr;

    lv_obj_t* eye_l_ = nullptr;
    lv_obj_t* eye_r_ = nullptr;
    lv_obj_t* pupil_l_ = nullptr;
    lv_obj_t* pupil_r_ = nullptr;
    lv_obj_t* glint_l_[2] = {};
    lv_obj_t* glint_r_[2] = {};
    bool blinkable_ = false;
    bool blinking_ = false;
    int blink_tick_ = 0;
    int eye_l_cx_ = 0;
    int eye_l_cy_ = 0;
    int eye_l_rx_ = 0;
    int eye_l_ry_ = 0;
    int eye_r_cx_ = 0;
    int eye_r_cy_ = 0;
    int eye_r_rx_ = 0;
    int eye_r_ry_ = 0;

    lv_obj_t* mouth_ = nullptr;
    int mouth_cx_ = 160;
    int mouth_cy_ = 154;
    int mouth_rx_ = 13;
    int mouth_ry_ = 11;
    bool mouth_open_ = true;

    lv_obj_t* wave_[2] = {};
    bool wave_on_ = true;
    lv_obj_t* think_dot_[3] = {};
    int think_step_ = 0;
    lv_timer_t* timer_ = nullptr;

    lv_point_precise_t antenna_pts_[2] = {};
    lv_point_precise_t pts_[160] = {};
    int pt_used_ = 0;
};
