#include "kodo_face_display.h"

#include "assets/lang_config.h"
#include "board.h"

#include <esp_random.h>

#include <cmath>
#include <cstring>

namespace {

const lv_color_t kBg = lv_color_hex(0x0B0F14);
const lv_color_t kEye = lv_color_hex(0x4FD1FF);
const lv_color_t kText = lv_color_hex(0xCDEFFF);

constexpr int kLeftX = 100;
constexpr int kRightX = 220;
constexpr int kRadius = 24;
constexpr int kBlinkHeight = 8;
constexpr int32_t kProgressMax = 1024;
constexpr uint32_t kTransitionMs = 220;
constexpr uint32_t kBlinkMs = 150;
constexpr uint32_t kBlinkGapMs = 3400;
constexpr uint32_t kBlinkJitterMs = 1200;
constexpr uint32_t kWakeAckMs = 800;
constexpr uint32_t kTickMs = 40;
constexpr lv_opa_t kDotsOpa = 153;

// Sad lids: background-coloured triangles that cut the outer top corner of each eye.
constexpr int kLidTop = 88;
constexpr int kLidBottom = 130;
constexpr int kLidHalf = 40;

lv_obj_t* Bare(lv_obj_t* parent) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    return obj;
}

lv_obj_t* Stroke(lv_obj_t* parent, const lv_point_precise_t* pts, uint32_t count, int width) {
    lv_obj_t* line = lv_line_create(parent);
    lv_line_set_points(line, pts, count);
    lv_obj_set_style_line_width(line, width, 0);
    lv_obj_set_style_line_color(line, kEye, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
    return line;
}

void PaintText(lv_obj_t* label) {
    if (label != nullptr) {
        lv_obj_set_style_text_color(label, kText, 0);
    }
}

void SetOpa(lv_obj_t* obj, lv_opa_t opa, bool line) {
    if (obj == nullptr) {
        return;
    }
    if (opa == LV_OPA_TRANSP) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (line) {
        lv_obj_set_style_line_opa(obj, opa, 0);
    } else {
        lv_obj_set_style_bg_opa(obj, opa, 0);
    }
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

int Lerp(int a, int b, int32_t t) { return a + (b - a) * t / kProgressMax; }

}  // namespace

KodoFaceDisplay::KodoFaceDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                                 int width, int height, int offset_x, int offset_y, bool mirror_x,
                                 bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                    swap_xy) {}

void KodoFaceDisplay::SetupUI() {
    if (setup_ui_called_) {
        return;
    }
    SpiLcdDisplay::SetupUI();

    DisplayLockGuard lock(this);
    ApplyPalette();
    if (emoji_label_ != nullptr) {
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (emoji_image_ != nullptr) {
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }
    if (emoji_box_ != nullptr) {
        lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    }

    canvas_ = Bare(lv_screen_active());
    lv_obj_set_size(canvas_, 320, 240);
    lv_obj_align(canvas_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(canvas_, kBg, 0);
    lv_obj_set_style_bg_opa(canvas_, LV_OPA_COVER, 0);
    BuildFace();

    for (lv_obj_t* layer :
         {top_bar_, status_bar_, bottom_bar_, preview_image_, low_battery_popup_}) {
        if (layer != nullptr) {
            lv_obj_move_foreground(layer);
        }
    }

    current_ = Face::kIdle;
    from_ = to_ = shown_ = Target(Face::kIdle);
    progress_ = kProgressMax;
    next_blink_ms_ = lv_tick_get() + kBlinkGapMs;
    Apply();

    timer_ = lv_timer_create(TickThunk, kTickMs, this);
    ready_ = true;
}

void KodoFaceDisplay::SetTheme(Theme* theme) {
    SpiLcdDisplay::SetTheme(theme);
    if (setup_ui_called_) {
        DisplayLockGuard lock(this);
        ApplyPalette();
    }
}

void KodoFaceDisplay::ApplyPalette() {
    lv_obj_t* screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, kBg, 0);
    lv_obj_set_style_text_color(screen, kText, 0);
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
}

void KodoFaceDisplay::BuildFace() {
    for (int i = 0; i < 2; i++) {
        eye_[i] = Bare(canvas_);
        lv_obj_set_style_bg_color(eye_[i], kEye, 0);
    }

    // Happy: thick quadratic arcs M(cx-32,136) Q(cx,70) (cx+32,136).
    constexpr int kSteps = 12;
    const int centers[2] = {kLeftX, kRightX};
    for (int i = 0; i < 2; i++) {
        const int cx = centers[i];
        for (int s = 0; s <= kSteps; s++) {
            float t = static_cast<float>(s) / kSteps;
            float u = 1.f - t;
            smile_pts_[i][s].x = static_cast<lv_value_precise_t>(
                u * u * (cx - 32) + 2 * u * t * cx + t * t * (cx + 32) + 0.5f);
            smile_pts_[i][s].y =
                static_cast<lv_value_precise_t>(u * u * 136 + 2 * u * t * 70 + t * t * 136 + 0.5f);
        }
        smile_[i] = Stroke(canvas_, smile_pts_[i], kSteps + 1, 20);
    }

    // Think: three rising dots towards the upper right.
    const int dots[3][3] = {{276, 60, 4}, {288, 46, 5}, {302, 30, 6}};
    for (int i = 0; i < 3; i++) {
        dot_[i] = Bare(canvas_);
        lv_obj_set_size(dot_[i], dots[i][2] * 2, dots[i][2] * 2);
        lv_obj_set_pos(dot_[i], dots[i][0] - dots[i][2], dots[i][1] - dots[i][2]);
        lv_obj_set_style_radius(dot_[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot_[i], kEye, 0);
        lv_obj_add_flag(dot_[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Sleep: two "Z" strokes, M x y h s l -s s h s.
    const int zz[2][3] = {{250, 70, 16}, {276, 44, 22}};
    for (int i = 0; i < 2; i++) {
        const int x = zz[i][0], y = zz[i][1], s = zz[i][2];
        zz_pts_[i][0] = {static_cast<lv_value_precise_t>(x), static_cast<lv_value_precise_t>(y)};
        zz_pts_[i][1] = {static_cast<lv_value_precise_t>(x + s),
                         static_cast<lv_value_precise_t>(y)};
        zz_pts_[i][2] = {static_cast<lv_value_precise_t>(x),
                         static_cast<lv_value_precise_t>(y + s)};
        zz_pts_[i][3] = {static_cast<lv_value_precise_t>(x + s),
                         static_cast<lv_value_precise_t>(y + s)};
        zz_[i] = Stroke(canvas_, zz_pts_[i], 4, s / 5 > 3 ? s / 5 : 3);
    }

    lv_obj_add_event_cb(canvas_, LidDrawThunk, LV_EVENT_DRAW_POST, this);
}

KodoFaceDisplay::Pose KodoFaceDisplay::Target(Face face) {
    Pose p = {};
    p.eye_opa = LV_OPA_COVER;
    auto both = [&p](int cy, int w, int h) {
        p.eye[0] = {kLeftX, static_cast<int16_t>(cy), static_cast<int16_t>(w),
                    static_cast<int16_t>(h), kRadius};
        p.eye[1] = {kRightX, static_cast<int16_t>(cy), static_cast<int16_t>(w),
                    static_cast<int16_t>(h), kRadius};
    };
    switch (face) {
        case Face::kIdle:
            both(120, 64, 84);
            break;
        case Face::kHappy:
            // The eyes squint and fade while the smile arcs fade in.
            both(116, 64, 20);
            p.eye_opa = LV_OPA_TRANSP;
            p.smile_opa = LV_OPA_COVER;
            break;
        case Face::kListen:
            p.eye[0] = {kLeftX, 114, 66, 92, kRadius};
            p.eye[1] = {kRightX, 112, 72, 100, kRadius};
            break;
        case Face::kSpeak:
            both(120, 66, 84);
            break;
        case Face::kThink:
            p.eye[0] = {kLeftX + 18, 104, 60, 70, kRadius};
            p.eye[1] = {kRightX + 18, 104, 60, 70, kRadius};
            p.dots_opa = LV_OPA_COVER;
            break;
        case Face::kSurprise:
            p.eye[0] = {96, 116, 88, 88, 44};
            p.eye[1] = {224, 116, 88, 88, 44};
            break;
        case Face::kSleep:
            both(134, 70, 10);
            p.zz_opa = LV_OPA_COVER;
            break;
        case Face::kSad:
            both(128, 64, 70);
            p.lid_opa = LV_OPA_COVER;
            break;
    }
    return p;
}

bool KodoFaceDisplay::Blinks(Face face) {
    return face == Face::kIdle || face == Face::kListen || face == Face::kThink;
}

void KodoFaceDisplay::Show(Face face) {
    if (face == current_) {
        return;
    }
    current_ = face;
    lv_anim_delete(this, BlinkExec);
    blinking_ = false;
    blink_ = 0;
    lv_anim_delete(this, TransitionExec);
    from_ = shown_;
    to_ = Target(face);
    progress_ = 0;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, this);
    lv_anim_set_exec_cb(&a, TransitionExec);
    lv_anim_set_values(&a, 0, kProgressMax);
    lv_anim_set_duration(&a, kTransitionMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

void KodoFaceDisplay::TransitionExec(void* var, int32_t value) {
    auto* self = static_cast<KodoFaceDisplay*>(var);
    self->progress_ = value;
    if (value >= kProgressMax) {
        uint32_t now = lv_tick_get();
        self->speak_start_ms_ = now;
        self->next_blink_ms_ = now + kBlinkGapMs / 2 + esp_random() % kBlinkJitterMs;
    }
    self->Apply();
}

void KodoFaceDisplay::StartBlink() {
    blinking_ = true;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, this);
    lv_anim_set_exec_cb(&a, BlinkExec);
    lv_anim_set_values(&a, 0, 2 * kProgressMax);
    lv_anim_set_duration(&a, kBlinkMs);
    lv_anim_start(&a);
}

void KodoFaceDisplay::BlinkExec(void* var, int32_t value) {
    auto* self = static_cast<KodoFaceDisplay*>(var);
    self->blink_ = value <= kProgressMax ? value : 2 * kProgressMax - value;
    if (value >= 2 * kProgressMax) {
        self->blinking_ = false;
        self->blink_ = 0;
        self->next_blink_ms_ = lv_tick_get() + kBlinkGapMs + esp_random() % kBlinkJitterMs;
    }
    self->Apply();
}

void KodoFaceDisplay::Apply() {
    Pose p;
    for (int i = 0; i < 2; i++) {
        const Eye& a = from_.eye[i];
        const Eye& b = to_.eye[i];
        p.eye[i].cx = Lerp(a.cx, b.cx, progress_);
        p.eye[i].cy = Lerp(a.cy, b.cy, progress_);
        p.eye[i].w = Lerp(a.w, b.w, progress_);
        p.eye[i].h = Lerp(a.h, b.h, progress_);
        p.eye[i].max_radius = Lerp(a.max_radius, b.max_radius, progress_);
    }
    p.eye_opa = Lerp(from_.eye_opa, to_.eye_opa, progress_);
    p.smile_opa = Lerp(from_.smile_opa, to_.smile_opa, progress_);
    p.lid_opa = Lerp(from_.lid_opa, to_.lid_opa, progress_);
    p.dots_opa = Lerp(from_.dots_opa, to_.dots_opa, progress_);
    p.zz_opa = Lerp(from_.zz_opa, to_.zz_opa, progress_);

    if (progress_ >= kProgressMax) {
        if (blink_ > 0 && Blinks(current_)) {
            for (Eye& e : p.eye) {
                e.h = Lerp(e.h, kBlinkHeight, blink_);
            }
        } else if (current_ == Face::kSpeak) {
            // Matches the design preview: 84 - 26 * |sin(0.9 * t)| with t in 110 ms frames.
            float t = static_cast<float>(lv_tick_elaps(speak_start_ms_)) * 0.9f / 110.f;
            int h = 84 - static_cast<int>(26.f * std::fabs(std::sin(t)) + 0.5f);
            for (Eye& e : p.eye) {
                e.h = h;
            }
        }
    }
    shown_ = p;
    Render(p);
}

void KodoFaceDisplay::Render(const Pose& p) {
    const bool force = !rendered_valid_;
    for (int i = 0; i < 2; i++) {
        const Eye& e = p.eye[i];
        const Eye& old = rendered_.eye[i];
        if (force || e.w != old.w || e.h != old.h) {
            lv_obj_set_size(eye_[i], e.w, e.h);
        }
        if (force || e.cx != old.cx || e.cy != old.cy || e.w != old.w || e.h != old.h) {
            lv_obj_set_pos(eye_[i], e.cx - e.w / 2, e.cy - e.h / 2);
        }
        int r = (e.w < e.h ? e.w : e.h) / 2;
        int old_r = (old.w < old.h ? old.w : old.h) / 2;
        r = r < e.max_radius ? r : e.max_radius;
        old_r = old_r < old.max_radius ? old_r : old.max_radius;
        if (force || r != old_r) {
            lv_obj_set_style_radius(eye_[i], r, 0);
        }
    }
    if (force || p.eye_opa != rendered_.eye_opa) {
        SetOpa(eye_[0], p.eye_opa, false);
        SetOpa(eye_[1], p.eye_opa, false);
    }
    if (force || p.smile_opa != rendered_.smile_opa) {
        SetOpa(smile_[0], p.smile_opa, true);
        SetOpa(smile_[1], p.smile_opa, true);
    }
    if (force || p.dots_opa != rendered_.dots_opa) {
        for (lv_obj_t* dot : dot_) {
            SetOpa(dot, p.dots_opa * kDotsOpa / LV_OPA_COVER, false);
        }
    }
    if (force || p.zz_opa != rendered_.zz_opa) {
        SetOpa(zz_[0], p.zz_opa, true);
        SetOpa(zz_[1], p.zz_opa, true);
    }
    if (force || p.lid_opa != rendered_.lid_opa) {
        lv_area_t area;
        lv_obj_get_coords(canvas_, &area);
        const int32_t x0 = area.x1;
        const int32_t y0 = area.y1;
        area.x1 = x0 + kLeftX - kLidHalf - 1;
        area.x2 = x0 + kRightX + kLidHalf + 1;
        area.y1 = y0 + kLidTop - 1;
        area.y2 = y0 + kLidBottom + 1;
        lv_obj_invalidate_area(canvas_, &area);
    }
    rendered_ = p;
    rendered_valid_ = true;
}

void KodoFaceDisplay::LidDrawThunk(lv_event_t* e) {
    auto* self = static_cast<KodoFaceDisplay*>(lv_event_get_user_data(e));
    const lv_opa_t opa = self->rendered_.lid_opa;
    if (opa == LV_OPA_TRANSP) {
        return;
    }
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_area_t area;
    lv_obj_get_coords(self->canvas_, &area);
    const int32_t x0 = area.x1;
    const int32_t y0 = area.y1;
    const int32_t lids[2][3][2] = {
        {{kLeftX - kLidHalf, kLidTop},
         {kLeftX + kLidHalf, kLidTop},
         {kLeftX - kLidHalf, kLidBottom}},
        {{kRightX - kLidHalf, kLidTop},
         {kRightX + kLidHalf, kLidTop},
         {kRightX + kLidHalf, kLidBottom}},
    };
    for (const auto& lid : lids) {
        lv_draw_triangle_dsc_t dsc;
        lv_draw_triangle_dsc_init(&dsc);
        dsc.color = kBg;
        dsc.opa = opa;
        for (int i = 0; i < 3; i++) {
            dsc.p[i].x = x0 + lid[i][0];
            dsc.p[i].y = y0 + lid[i][1];
        }
        lv_draw_triangle(layer, &dsc);
    }
}

void KodoFaceDisplay::SetStatus(const char* status) {
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

bool KodoFaceDisplay::SetEmotion(const char* emotion) {
    if (!ready_ || emotion == nullptr || emotion[0] == '\0') {
        return false;
    }
    DisplayLockGuard lock(this);
    if (acking_) {
        return true;
    }
    if (strcmp(emotion, "neutral") == 0 || strcmp(emotion, "robot_2") == 0) {
        Show(sleeping_ ? Face::kSleep : mode_);
        return true;
    }
    Face face;
    if (!MapEmotion(emotion, &face)) {
        return false;
    }
    Show(face);
    return true;
}

void KodoFaceDisplay::SetPowerSaveMode(bool on) {
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

void KodoFaceDisplay::ArmWakeAck(Face next) {
    mode_ = next;
    acking_ = true;
    ack_start_ms_ = lv_tick_get();
    Show(Face::kSurprise);
}

void KodoFaceDisplay::LeaveSleep() {
    if (!sleeping_) {
        return;
    }
    sleeping_ = false;
    auto* backlight = Board::GetInstance().GetBacklight();
    if (backlight != nullptr) {
        backlight->RestoreBrightness();
    }
}

void KodoFaceDisplay::TickThunk(lv_timer_t* timer) {
    static_cast<KodoFaceDisplay*>(lv_timer_get_user_data(timer))->OnTick();
}

void KodoFaceDisplay::OnTick() {
    if (acking_ && lv_tick_elaps(ack_start_ms_) >= kWakeAckMs) {
        acking_ = false;
        Show(mode_);
    }
    if (progress_ < kProgressMax) {
        return;
    }
    if (current_ == Face::kSpeak) {
        Apply();
    } else if (Blinks(current_) && !blinking_ &&
               static_cast<int32_t>(lv_tick_get() - next_blink_ms_) >= 0) {
        StartBlink();
    }
}

bool KodoFaceDisplay::ClassifyStatus(const char* status, Face* face) const {
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
        *face = Face::kSad;
        return true;
    }
    return false;
}

bool KodoFaceDisplay::MapEmotion(const char* emotion, Face* face) const {
    struct Item {
        const char* name;
        Face face;
    };
    static constexpr Item kMap[] = {
        {"idle", Face::kIdle},         {"listen", Face::kListen},      {"listening", Face::kListen},
        {"look", Face::kListen},       {"speak", Face::kSpeak},        {"speaking", Face::kSpeak},
        {"think", Face::kThink},       {"thinking", Face::kThink},     {"confuse", Face::kThink},
        {"confused", Face::kThink},    {"happy", Face::kHappy},        {"laughing", Face::kHappy},
        {"funny", Face::kHappy},       {"cool", Face::kHappy},         {"confident", Face::kHappy},
        {"relaxed", Face::kHappy},     {"delicious", Face::kHappy},    {"silly", Face::kHappy},
        {"wink", Face::kHappy},        {"winking", Face::kHappy},      {"love", Face::kHappy},
        {"loving", Face::kHappy},      {"kissy", Face::kHappy},        {"shy", Face::kHappy},
        {"surprise", Face::kSurprise}, {"surprised", Face::kSurprise}, {"shocked", Face::kSurprise},
        {"sleep", Face::kSleep},       {"sleepy", Face::kSleep},       {"sad", Face::kSad},
        {"crying", Face::kSad},        {"embarrassed", Face::kSad},    {"oops", Face::kSad},
        {"angry", Face::kSad},
    };
    for (const Item& item : kMap) {
        if (strcmp(emotion, item.name) == 0) {
            *face = item.face;
            return true;
        }
    }
    return false;
}
