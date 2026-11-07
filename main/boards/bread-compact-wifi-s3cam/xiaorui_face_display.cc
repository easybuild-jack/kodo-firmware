#include "xiaorui_face_display.h"

#include "assets/lang_config.h"
#include "board.h"

#include <cstring>

namespace {

const lv_color_t kLine = lv_color_hex(0x4C8DFF);
const lv_color_t kLineSoft = lv_color_hex(0x8BB6FF);
const lv_color_t kBg = lv_color_hex(0xEAF3FF);
const lv_color_t kFace = lv_color_hex(0xFFFAF7);
const lv_color_t kEar = lv_color_hex(0xF0F6FF);
const lv_color_t kWhite = lv_color_hex(0xFFFFFF);
const lv_color_t kPupil = lv_color_hex(0x2A4A7A);
const lv_color_t kBlush = lv_color_hex(0xFFB7C8);
const lv_color_t kPink = lv_color_hex(0xFF8FAB);
const lv_color_t kInk = lv_color_hex(0x2A4A7A);

lv_obj_t* Bare(lv_obj_t* parent) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    return obj;
}

void PaintText(lv_obj_t* label) {
    if (label != nullptr) {
        lv_obj_set_style_text_color(label, kInk, 0);
    }
}

}  // namespace

XiaoruiFaceDisplay::XiaoruiFaceDisplay(esp_lcd_panel_io_handle_t panel_io,
                                       esp_lcd_panel_handle_t panel, int width, int height,
                                       int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                                       bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                    swap_xy) {}

void XiaoruiFaceDisplay::SetupUI() {
    if (setup_ui_called_) {
        return;
    }
    SpiLcdDisplay::SetupUI();

    DisplayLockGuard lock(this);
    lv_obj_t* screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, kBg, 0);
    lv_obj_set_style_text_color(screen, kInk, 0);
    if (container_ != nullptr) {
        lv_obj_set_style_bg_opa(container_, LV_OPA_TRANSP, 0);
    }
    if (top_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
    }
    if (status_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_set_style_bg_color(bottom_bar_, kBg, 0);
        lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_70, 0);
    }
    PaintText(network_label_);
    PaintText(status_label_);
    if (status_label_ != nullptr) {
        lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(status_label_, LV_ALIGN_RIGHT_MID, -8, 0);
    }
    PaintText(notification_label_);
    PaintText(mute_label_);
    PaintText(battery_label_);
    PaintText(chat_message_label_);
    if (emoji_label_ != nullptr) {
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (emoji_image_ != nullptr) {
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }
    if (emoji_box_ != nullptr) {
        lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    }

    canvas_ = Bare(screen);
    lv_obj_set_size(canvas_, 320, 240);
    lv_obj_align(canvas_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(canvas_, kBg, 0);
    lv_obj_set_style_bg_opa(canvas_, LV_OPA_COVER, 0);
    BuildChassis();
    Show(Face::kIdle);

    for (lv_obj_t* layer :
         {top_bar_, status_bar_, bottom_bar_, preview_image_, low_battery_popup_}) {
        if (layer != nullptr) {
            lv_obj_move_foreground(layer);
        }
    }

    timer_ = lv_timer_create(TickThunk, 100, this);
    ready_ = true;
}

void XiaoruiFaceDisplay::SetStatus(const char* status) {
    SpiLcdDisplay::SetStatus(status);
    if (!ready_ || status == nullptr) {
        return;
    }
    Face face;
    if (!ClassifyStatus(status, &face)) {
        return;
    }
    DisplayLockGuard lock(this);
    if (sleeping_) {
        if (face == Face::kSpeak) {
            Show(Face::kSpeak);
            return;
        }
        if (face == Face::kIdle) {
            mode_ = Face::kSleep;
            Show(Face::kSleep);
            return;
        }
        LeaveSleep();
    }
    if (acking_ && face == Face::kListen) {
        mode_ = Face::kListen;
        return;
    }
    const bool resting = mode_ == Face::kIdle || mode_ == Face::kSleep || current_ == Face::kIdle ||
                         current_ == Face::kSleep;
    if (resting && (face == Face::kListen || face == Face::kSurprise)) {
        ArmWakeAck(face == Face::kListen ? Face::kListen : Face::kSurprise);
        return;
    }
    acking_ = false;
    mode_ = face;
    Show(face);
}

bool XiaoruiFaceDisplay::SetEmotion(const char* emotion) {
    if (!ready_ || emotion == nullptr || emotion[0] == '\0') {
        return false;
    }
    DisplayLockGuard lock(this);
    if (acking_) {
        return true;
    }
    if (strcmp(emotion, "neutral") == 0 || strcmp(emotion, "robot_2") == 0) {
        Show(sleeping_ ? Face::kSleep : FaceForMode());
        return true;
    }
    Face face;
    if (!MapEmotion(emotion, &face)) {
        return false;
    }
    if (sleeping_ && face == Face::kSleep) {
        Show(Face::kSleep);
        return true;
    }
    Show(face);
    return true;
}

void XiaoruiFaceDisplay::SetPowerSaveMode(bool on) {
    SetChatMessage("system", "");
    if (on) {
        sleeping_ = true;
        acking_ = false;
        if (ready_) {
            DisplayLockGuard lock(this);
            mode_ = Face::kSleep;
            Show(Face::kSleep);
        }
        auto* backlight = Board::GetInstance().GetBacklight();
        if (backlight != nullptr) {
            backlight->SetBrightness(20, false);
        }
        return;
    }
    LeaveSleep();
    if (ready_) {
        DisplayLockGuard lock(this);
        mode_ = Face::kIdle;
        Show(Face::kIdle);
    }
}

void XiaoruiFaceDisplay::ArmWakeAck(Face next) {
    mode_ = next;
    acking_ = true;
    ack_ticks_ = 0;
    Show(Face::kSurprise);
}

void XiaoruiFaceDisplay::LeaveSleep() {
    if (!sleeping_) {
        return;
    }
    sleeping_ = false;
    auto* backlight = Board::GetInstance().GetBacklight();
    if (backlight != nullptr) {
        backlight->RestoreBrightness();
    }
}

void XiaoruiFaceDisplay::TickThunk(lv_timer_t* timer) {
    static_cast<XiaoruiFaceDisplay*>(lv_timer_get_user_data(timer))->OnTick();
}

void XiaoruiFaceDisplay::OnTick() {
    if (acking_ && ++ack_ticks_ >= 8) {
        acking_ = false;
        Show(mode_);
    }
    if (current_ == Face::kIdle && blinkable_ && eye_l_ != nullptr && eye_r_ != nullptr &&
        pupil_l_ != nullptr && pupil_r_ != nullptr && glint_l_[0] != nullptr &&
        glint_l_[1] != nullptr && glint_r_[0] != nullptr && glint_r_[1] != nullptr) {
        blink_tick_++;
        if (!blinking_ && blink_tick_ >= 50) {
            blinking_ = true;
            blink_tick_ = 0;
            lv_obj_set_size(eye_l_, eye_l_rx_ * 2, 6);
            lv_obj_set_pos(eye_l_, eye_l_cx_ - eye_l_rx_, eye_l_cy_ - 3);
            lv_obj_set_size(eye_r_, eye_r_rx_ * 2, 6);
            lv_obj_set_pos(eye_r_, eye_r_cx_ - eye_r_rx_, eye_r_cy_ - 3);
            lv_obj_add_flag(pupil_l_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(pupil_r_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(glint_l_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(glint_l_[1], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(glint_r_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(glint_r_[1], LV_OBJ_FLAG_HIDDEN);
        } else if (blinking_ && blink_tick_ >= 2) {
            blinking_ = false;
            blink_tick_ = 0;
            lv_obj_set_size(eye_l_, eye_l_rx_ * 2, eye_l_ry_ * 2);
            lv_obj_set_pos(eye_l_, eye_l_cx_ - eye_l_rx_, eye_l_cy_ - eye_l_ry_);
            lv_obj_set_size(eye_r_, eye_r_rx_ * 2, eye_r_ry_ * 2);
            lv_obj_set_pos(eye_r_, eye_r_cx_ - eye_r_rx_, eye_r_cy_ - eye_r_ry_);
            lv_obj_remove_flag(pupil_l_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(pupil_r_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(glint_l_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(glint_l_[1], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(glint_r_[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(glint_r_[1], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (current_ == Face::kListen && wave_[0] != nullptr) {
        blink_tick_++;
        if (blink_tick_ >= 4) {
            blink_tick_ = 0;
            wave_on_ = !wave_on_;
            if (wave_on_) {
                lv_obj_remove_flag(wave_[0], LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(wave_[1], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(wave_[0], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(wave_[1], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    if (current_ == Face::kSpeak && mouth_ != nullptr) {
        mouth_open_ = !mouth_open_;
        int rx = mouth_open_ ? mouth_rx_ : 8;
        int ry = mouth_open_ ? mouth_ry_ : 6;
        lv_obj_set_size(mouth_, rx * 2, ry * 2);
        lv_obj_set_pos(mouth_, mouth_cx_ - rx, mouth_cy_ - ry);
        if (chin_mid_ != nullptr) {
            lv_obj_set_style_bg_opa(chin_mid_, mouth_open_ ? LV_OPA_COVER : LV_OPA_40, 0);
        }
    }

    if (current_ == Face::kThink && think_dot_[0] != nullptr) {
        blink_tick_++;
        if (blink_tick_ >= 3) {
            blink_tick_ = 0;
            think_step_ = (think_step_ + 1) % 3;
            for (int i = 0; i < 3; i++) {
                lv_obj_set_style_bg_opa(think_dot_[i], i == think_step_ ? LV_OPA_COVER : LV_OPA_30,
                                        0);
            }
        }
    }

    if (current_ == Face::kSurprise && antenna_ball_ != nullptr) {
        blink_tick_++;
        if (blink_tick_ >= 3) {
            blink_tick_ = 0;
            wave_on_ = !wave_on_;
            lv_obj_set_style_bg_opa(antenna_ball_, wave_on_ ? LV_OPA_COVER : LV_OPA_40, 0);
        }
    }
}

bool XiaoruiFaceDisplay::ClassifyStatus(const char* status, Face* face) const {
    if (strcmp(status, Lang::Strings::LISTENING) == 0) {
        *face = Face::kListen;
        return true;
    }
    if (strcmp(status, Lang::Strings::SPEAKING) == 0) {
        *face = Face::kSpeak;
        return true;
    }
    if (strcmp(status, Lang::Strings::CONNECTING) == 0) {
        *face = Face::kSurprise;
        return true;
    }
    if (strcmp(status, Lang::Strings::STANDBY) == 0) {
        *face = Face::kIdle;
        return true;
    }
    if (strcmp(status, Lang::Strings::ERROR) == 0) {
        *face = Face::kOops;
        return true;
    }
    return false;
}

XiaoruiFaceDisplay::Face XiaoruiFaceDisplay::FaceForMode() const { return mode_; }

bool XiaoruiFaceDisplay::MapEmotion(const char* emotion, Face* face) const {
    struct Item {
        const char* name;
        Face face;
    };
    static constexpr Item kMap[] = {
        {"idle", Face::kIdle},
        {"listen", Face::kListen},
        {"listening", Face::kListen},
        {"speak", Face::kSpeak},
        {"speaking", Face::kSpeak},
        {"think", Face::kThink},
        {"thinking", Face::kThink},
        {"happy", Face::kHappy},
        {"laughing", Face::kHappy},
        {"funny", Face::kHappy},
        {"cool", Face::kHappy},
        {"confident", Face::kHappy},
        {"relaxed", Face::kHappy},
        {"delicious", Face::kHappy},
        {"silly", Face::kHappy},
        {"wink", Face::kWink},
        {"winking", Face::kWink},
        {"surprise", Face::kSurprise},
        {"surprised", Face::kSurprise},
        {"shocked", Face::kSurprise},
        {"confuse", Face::kConfuse},
        {"confused", Face::kConfuse},
        {"sad", Face::kSad},
        {"crying", Face::kSad},
        {"sleep", Face::kSleep},
        {"sleepy", Face::kSleep},
        {"look", Face::kLook},
        {"love", Face::kLove},
        {"loving", Face::kLove},
        {"kissy", Face::kLove},
        {"shy", Face::kShy},
        {"embarrassed", Face::kShy},
        {"oops", Face::kOops},
        {"angry", Face::kOops},
    };
    for (const Item& item : kMap) {
        if (strcmp(emotion, item.name) == 0) {
            *face = item.face;
            return true;
        }
    }
    return false;
}

void XiaoruiFaceDisplay::BuildChassis() {
    antenna_pts_[0] = {160, 28};
    antenna_pts_[1] = {160, 46};
    lv_obj_t* stem = lv_line_create(canvas_);
    lv_line_set_points(stem, antenna_pts_, 2);
    lv_obj_set_style_line_width(stem, 3, 0);
    lv_obj_set_style_line_color(stem, kLine, 0);
    lv_obj_set_style_line_rounded(stem, true, 0);

    antenna_ball_ = Dot(canvas_, 160, 22, 8, kLine, LV_OPA_COVER);
    Dot(canvas_, 162, 19, 2, kWhite, 204);

    Chip(canvas_, 30, 90, 26, 52, 13, kEar, kLine, 3);
    Chip(canvas_, 264, 90, 26, 52, 13, kEar, kLine, 3);
    Dot(canvas_, 43, 104, 3, kLineSoft, LV_OPA_COVER);
    Dot(canvas_, 277, 104, 3, kLineSoft, LV_OPA_COVER);

    Chip(canvas_, 60, 50, 200, 152, 36, kFace, kLine, 3);
    Dot(canvas_, 148, 186, 3, kLineSoft, 178);
    chin_mid_ = Dot(canvas_, 160, 186, 3, kLine, 217);
    Dot(canvas_, 172, 186, 3, kLineSoft, 178);
    Dot(canvas_, 76, 66, 2, kLineSoft, LV_OPA_COVER);
    Dot(canvas_, 244, 66, 2, kLineSoft, LV_OPA_COVER);
}

void XiaoruiFaceDisplay::ClearFeatures() {
    eye_l_ = eye_r_ = nullptr;
    pupil_l_ = pupil_r_ = nullptr;
    glint_l_[0] = glint_l_[1] = nullptr;
    glint_r_[0] = glint_r_[1] = nullptr;
    mouth_ = nullptr;
    wave_[0] = wave_[1] = nullptr;
    think_dot_[0] = think_dot_[1] = think_dot_[2] = nullptr;
    blinkable_ = false;
    blinking_ = false;
    blink_tick_ = 0;
    if (features_ != nullptr) {
        lv_obj_del(features_);
        features_ = nullptr;
    }
    pt_used_ = 0;
    features_ = Bare(canvas_);
    lv_obj_set_size(features_, 320, 240);
    lv_obj_set_pos(features_, 0, 0);
}

lv_obj_t* XiaoruiFaceDisplay::Dot(lv_obj_t* parent, int cx, int cy, int r, lv_color_t fill,
                                  lv_opa_t opa) {
    lv_obj_t* obj = Bare(parent);
    lv_obj_set_size(obj, r * 2, r * 2);
    lv_obj_set_pos(obj, cx - r, cy - r);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(obj, fill, 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
    return obj;
}

lv_obj_t* XiaoruiFaceDisplay::Oval(lv_obj_t* parent, int cx, int cy, int rx, int ry,
                                   lv_color_t fill, lv_opa_t fill_opa, lv_color_t stroke,
                                   int stroke_w) {
    lv_obj_t* obj = Bare(parent);
    lv_obj_set_size(obj, rx * 2, ry * 2);
    lv_obj_set_pos(obj, cx - rx, cy - ry);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(obj, fill, 0);
    lv_obj_set_style_bg_opa(obj, fill_opa, 0);
    if (stroke_w > 0) {
        lv_obj_set_style_border_width(obj, stroke_w, 0);
        lv_obj_set_style_border_color(obj, stroke, 0);
    }
    return obj;
}

lv_obj_t* XiaoruiFaceDisplay::Chip(lv_obj_t* parent, int x, int y, int w, int h, int radius,
                                   lv_color_t fill, lv_color_t stroke, int stroke_w) {
    lv_obj_t* obj = Bare(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_bg_color(obj, fill, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, stroke_w, 0);
    lv_obj_set_style_border_color(obj, stroke, 0);
    return obj;
}

const lv_point_precise_t* XiaoruiFaceDisplay::AddQuad(int x0, int y0, int cx, int cy, int x1,
                                                      int y1, uint32_t* count) {
    constexpr int kSteps = 8;
    if (pt_used_ + kSteps + 1 > static_cast<int>(sizeof(pts_) / sizeof(pts_[0]))) {
        *count = 0;
        return nullptr;
    }
    int start = pt_used_;
    for (int i = 0; i <= kSteps; i++) {
        float t = static_cast<float>(i) / kSteps;
        float u = 1.f - t;
        pts_[pt_used_].x = static_cast<int>(u * u * x0 + 2 * u * t * cx + t * t * x1 + 0.5f);
        pts_[pt_used_].y = static_cast<int>(u * u * y0 + 2 * u * t * cy + t * t * y1 + 0.5f);
        pt_used_++;
    }
    *count = kSteps + 1;
    return &pts_[start];
}

const lv_point_precise_t* XiaoruiFaceDisplay::AddSeg(int x0, int y0, int x1, int y1,
                                                     uint32_t* count) {
    if (pt_used_ + 2 > static_cast<int>(sizeof(pts_) / sizeof(pts_[0]))) {
        *count = 0;
        return nullptr;
    }
    int start = pt_used_;
    pts_[pt_used_].x = x0;
    pts_[pt_used_].y = y0;
    pt_used_++;
    pts_[pt_used_].x = x1;
    pts_[pt_used_].y = y1;
    pt_used_++;
    *count = 2;
    return &pts_[start];
}

lv_obj_t* XiaoruiFaceDisplay::Stroke(const lv_point_precise_t* points, uint32_t count, int width,
                                     lv_color_t color) {
    if (points == nullptr || count < 2) {
        return nullptr;
    }
    lv_obj_t* line = lv_line_create(features_);
    lv_line_set_points(line, points, count);
    lv_obj_set_style_line_width(line, width, 0);
    lv_obj_set_style_line_color(line, color, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    return line;
}

void XiaoruiFaceDisplay::OpenEye(int cx, int cy, int rx, int ry, int pupil_r, int pupil_dx,
                                 int pupil_dy, bool left) {
    lv_obj_t* white = Oval(features_, cx, cy, rx, ry, kWhite, LV_OPA_COVER, kLine, 3);
    int px = cx + pupil_dx;
    int py = cy + pupil_dy;
    lv_obj_t* pupil = Dot(features_, px, py, pupil_r, kPupil, LV_OPA_COVER);
    lv_obj_t* hi = Dot(features_, px + 3, py - 3, 2, kWhite, LV_OPA_COVER);
    lv_obj_t* lo = Dot(features_, px - 3, py + 2, 1, kWhite, 178);
    if (left) {
        eye_l_ = white;
        pupil_l_ = pupil;
        glint_l_[0] = hi;
        glint_l_[1] = lo;
        eye_l_cx_ = cx;
        eye_l_cy_ = cy;
        eye_l_rx_ = rx;
        eye_l_ry_ = ry;
    } else {
        eye_r_ = white;
        pupil_r_ = pupil;
        glint_r_[0] = hi;
        glint_r_[1] = lo;
        eye_r_cx_ = cx;
        eye_r_cy_ = cy;
        eye_r_rx_ = rx;
        eye_r_ry_ = ry;
    }
}

void XiaoruiFaceDisplay::Crescent(int x0, int y0, int cx, int cy, int x1, int y1, int width) {
    uint32_t count = 0;
    const lv_point_precise_t* pts = AddQuad(x0, y0, cx, cy, x1, y1, &count);
    Stroke(pts, count, width, kLine);
}

void XiaoruiFaceDisplay::Blush(lv_opa_t opa) {
    Oval(features_, 84, 138, 16, 10, kBlush, opa, kBlush, 0);
    Oval(features_, 236, 138, 16, 10, kBlush, opa, kBlush, 0);
}

void XiaoruiFaceDisplay::Show(Face face) {
    if (antenna_ball_ != nullptr) {
        lv_obj_set_style_bg_opa(antenna_ball_, LV_OPA_COVER, 0);
    }
    ClearFeatures();
    current_ = face;

    uint32_t count = 0;
    switch (face) {
        case Face::kIdle:
            Blush(102);
            OpenEye(114, 108, 18, 20, 7, 0, 1, true);
            OpenEye(206, 108, 18, 20, 7, 0, 1, false);
            blinkable_ = true;
            Crescent(146, 150, 160, 162, 174, 150, 4);
            break;
        case Face::kListen:
            Blush(102);
            OpenEye(114, 108, 20, 22, 8, 0, 1, true);
            OpenEye(206, 108, 20, 22, 8, 0, 1, false);
            mouth_ = Oval(features_, 160, 154, 7, 5, kPink, 178, kLine, 2);
            Stroke(AddQuad(48, 100, 40, 120, 48, 140, &count), count, 3, kLineSoft);
            wave_[0] = Stroke(AddQuad(38, 92, 26, 120, 38, 148, &count), count, 3, kLineSoft);
            Stroke(AddQuad(272, 100, 280, 120, 272, 140, &count), count, 3, kLineSoft);
            wave_[1] = Stroke(AddQuad(282, 92, 294, 120, 282, 148, &count), count, 3, kLineSoft);
            break;
        case Face::kSpeak:
            Blush(102);
            OpenEye(114, 108, 18, 20, 7, 0, 1, true);
            OpenEye(206, 108, 18, 20, 7, 0, 1, false);
            mouth_cx_ = 160;
            mouth_cy_ = 154;
            mouth_rx_ = 13;
            mouth_ry_ = 11;
            mouth_open_ = true;
            mouth_ =
                Oval(features_, mouth_cx_, mouth_cy_, mouth_rx_, mouth_ry_, kPink, 191, kLine, 2);
            break;
        case Face::kThink:
            Blush(102);
            OpenEye(114, 108, 18, 19, 7, 5, -3, true);
            OpenEye(206, 108, 18, 19, 7, 5, -3, false);
            Stroke(AddSeg(150, 150, 170, 150, &count), count, 3, kLine);
            think_dot_[0] = Dot(features_, 248, 56, 4, kLine, LV_OPA_COVER);
            think_dot_[1] = Dot(features_, 262, 44, 6, kLine, LV_OPA_30);
            think_dot_[2] = Dot(features_, 278, 30, 8, kLine, LV_OPA_30);
            break;
        case Face::kHappy:
            Blush(217);
            Crescent(96, 110, 114, 94, 132, 110, 4);
            Crescent(188, 110, 206, 94, 224, 110, 4);
            Crescent(140, 148, 160, 168, 180, 148, 4);
            break;
        case Face::kWink:
            Blush(217);
            Crescent(96, 110, 114, 94, 132, 110, 4);
            Stroke(AddSeg(190, 108, 222, 108, &count), count, 4, kLine);
            Crescent(140, 148, 160, 168, 180, 148, 4);
            Dot(features_, 236, 98, 4, kLineSoft, LV_OPA_COVER);
            break;
        case Face::kSurprise:
            Blush(102);
            OpenEye(114, 108, 22, 24, 8, 0, 2, true);
            OpenEye(206, 108, 22, 24, 8, 0, 2, false);
            Oval(features_, 160, 152, 8, 10, kFace, LV_OPA_TRANSP, kLine, 3);
            break;
        case Face::kConfuse:
            Blush(102);
            OpenEye(114, 108, 18, 20, 7, 0, 1, true);
            OpenEye(206, 108, 22, 24, 8, 0, 2, false);
            Stroke(AddQuad(144, 150, 154, 158, 160, 150, &count), count, 3, kLine);
            Stroke(AddQuad(160, 150, 166, 142, 176, 150, &count), count, 3, kLine);
            {
                lv_obj_t* mark = lv_label_create(features_);
                lv_label_set_text(mark, "?");
                lv_obj_set_pos(mark, 248, 18);
                lv_obj_set_style_text_color(mark, kLine, 0);
            }
            break;
        case Face::kSad:
            Blush(102);
            OpenEye(114, 112, 18, 17, 7, 0, 1, true);
            OpenEye(206, 112, 18, 17, 7, 0, 1, false);
            Crescent(100, 96, 114, 102, 128, 96, 3);
            Crescent(192, 96, 206, 102, 220, 96, 3);
            Crescent(144, 158, 160, 146, 176, 158, 3);
            Oval(features_, 224, 136, 4, 7, kLineSoft, LV_OPA_COVER, kLineSoft, 0);
            break;
        case Face::kSleep:
            Crescent(98, 108, 114, 115, 130, 108, 4);
            Crescent(190, 108, 206, 115, 222, 108, 4);
            Crescent(152, 150, 160, 156, 168, 150, 3);
            {
                lv_obj_t* z1 = lv_label_create(features_);
                lv_label_set_text(z1, "z");
                lv_obj_set_pos(z1, 228, 34);
                lv_obj_set_style_text_color(z1, kLineSoft, 0);
                lv_obj_t* z2 = lv_label_create(features_);
                lv_label_set_text(z2, "z");
                lv_obj_set_pos(z2, 246, 18);
                lv_obj_set_style_text_color(z2, kLineSoft, 0);
            }
            break;
        case Face::kLook:
            OpenEye(114, 108, 20, 22, 9, 0, 1, true);
            OpenEye(206, 108, 20, 22, 9, 0, 1, false);
            Stroke(AddSeg(150, 150, 170, 150, &count), count, 3, kLine);
            Stroke(AddSeg(40, 40, 68, 40, &count), count, 3, kLine);
            Stroke(AddSeg(68, 40, 68, 54, &count), count, 3, kLine);
            Stroke(AddSeg(280, 40, 252, 40, &count), count, 3, kLine);
            Stroke(AddSeg(252, 40, 252, 54, &count), count, 3, kLine);
            Stroke(AddSeg(40, 200, 68, 200, &count), count, 3, kLine);
            Stroke(AddSeg(68, 200, 68, 186, &count), count, 3, kLine);
            Stroke(AddSeg(280, 200, 252, 200, &count), count, 3, kLine);
            Stroke(AddSeg(252, 200, 252, 186, &count), count, 3, kLine);
            break;
        case Face::kLove:
            Blush(217);
            Dot(features_, 106, 104, 8, kPink, LV_OPA_COVER);
            Dot(features_, 122, 104, 8, kPink, LV_OPA_COVER);
            Dot(features_, 114, 114, 10, kPink, LV_OPA_COVER);
            Dot(features_, 198, 104, 8, kPink, LV_OPA_COVER);
            Dot(features_, 214, 104, 8, kPink, LV_OPA_COVER);
            Dot(features_, 206, 114, 10, kPink, LV_OPA_COVER);
            Crescent(140, 148, 160, 168, 180, 148, 4);
            Dot(features_, 36, 78, 8, kPink, 230);
            Dot(features_, 284, 86, 8, kPink, 230);
            break;
        case Face::kShy:
            Blush(230);
            Crescent(96, 110, 114, 94, 132, 110, 4);
            OpenEye(206, 108, 17, 16, 6, -3, 1, false);
            mouth_ = Oval(features_, 160, 152, 6, 4, kPink, 204, kLine, 2);
            break;
        case Face::kOops:
            Stroke(AddSeg(102, 96, 126, 120, &count), count, 4, kLine);
            Stroke(AddSeg(126, 96, 102, 120, &count), count, 4, kLine);
            Stroke(AddSeg(194, 96, 218, 120, &count), count, 4, kLine);
            Stroke(AddSeg(218, 96, 194, 120, &count), count, 4, kLine);
            Stroke(AddQuad(144, 150, 154, 158, 160, 150, &count), count, 3, kLine);
            Stroke(AddQuad(160, 150, 166, 142, 176, 150, &count), count, 3, kLine);
            Oval(features_, 160, 30, 11, 11, kBg, LV_OPA_TRANSP, kLine, 3);
            Stroke(AddSeg(160, 24, 160, 32, &count), count, 3, kLine);
            Dot(features_, 160, 37, 2, kLine, LV_OPA_COVER);
            break;
    }
}
