#include "kodo_light.h"

#include "application.h"
#include "device_state_machine.h"
#include "mcp_server.h"
#include "settings.h"

#include <esp_log.h>

#include <algorithm>
#include <cassert>
#include <cmath>

#define TAG "KodoLight"

namespace {

// 16 pixels on USB power: every pixel value is scaled to this share of full scale.
constexpr int kOutputCapPercent = 30;

// Boot spinner color before the cap; after it, it matches the old CircularStrip spinner.
constexpr uint8_t kSpinnerHigh = 54;
constexpr uint8_t kSpinnerLow = 7;
constexpr int kSpinnerLength = 3;
constexpr int kSpinnerTickMs = 100;

constexpr int kBreatheTickMs = 20;
constexpr int kRainbowTickMs = 30;
constexpr int kFadeOutTickMs = 50;

constexpr char kSettingsNamespace[] = "kodo_light";

uint8_t Scale(uint8_t value, int percent) { return static_cast<uint8_t>(value * percent / 100); }

}  // namespace

KodoLight::KodoLight(gpio_num_t gpio, uint16_t max_leds) : max_leds_(max_leds) {
    assert(gpio != GPIO_NUM_NC);
    frame_.resize(max_leds_);

    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = gpio;
    strip_config.max_leds = max_leds_;
    strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    strip_config.led_model = LED_MODEL_WS2812;

    led_strip_rmt_config_t rmt_config = {};
    rmt_config.resolution_hz = 10 * 1000 * 1000;

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip_));
    led_strip_clear(led_strip_);

    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) { static_cast<KodoLight*>(arg)->OnTimer(); },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "kodo_light",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_));

    LoadSettings();
    RegisterTools();
    {
        // The board is built before Application enters kDeviceStateStarting and registers its
        // state listener, so that transition never reaches OnStateChanged; spin from here.
        std::lock_guard<std::mutex> lock(mutex_);
        StartModeLocked(Mode::kScroll, {kSpinnerLow, kSpinnerLow, kSpinnerHigh}, kSpinnerTickMs);
    }
    ESP_LOGI(TAG, "Ring ready: gpio=%d leds=%d cap=%d%%", gpio, max_leds_, kOutputCapPercent);
}

KodoLight::~KodoLight() {
    esp_timer_stop(timer_);
    esp_timer_delete(timer_);
    if (led_strip_ != nullptr) {
        led_strip_del(led_strip_);
    }
}

const char* KodoLight::EffectName(Effect effect) {
    switch (effect) {
        case Effect::kSolid:
            return "solid";
        case Effect::kBreathe:
            return "breathe";
        case Effect::kRainbow:
            return "rainbow";
        case Effect::kOff:
        default:
            return "off";
    }
}

bool KodoLight::ParseEffect(const std::string& name, Effect* effect) {
    if (name == "solid") {
        *effect = Effect::kSolid;
    } else if (name == "breathe") {
        *effect = Effect::kBreathe;
    } else if (name == "rainbow") {
        *effect = Effect::kRainbow;
    } else if (name == "off") {
        *effect = Effect::kOff;
    } else {
        return false;
    }
    return true;
}

void KodoLight::LoadSettings() {
    Settings settings(kSettingsNamespace);
    Ambient ambient;
    ambient.color.r = static_cast<uint8_t>(std::clamp<int32_t>(settings.GetInt("r", 255), 0, 255));
    ambient.color.g = static_cast<uint8_t>(std::clamp<int32_t>(settings.GetInt("g", 255), 0, 255));
    ambient.color.b = static_cast<uint8_t>(std::clamp<int32_t>(settings.GetInt("b", 255), 0, 255));
    ambient.brightness = std::clamp<int32_t>(settings.GetInt("bright", 50), 0, 100);
    ambient.speed_ms = std::clamp<int32_t>(settings.GetInt("speed", 1500), 200, 5000);
    if (!ParseEffect(settings.GetString("effect", "off"), &ambient.effect)) {
        ambient.effect = Effect::kOff;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    ambient_ = ambient;
    ESP_LOGI(TAG, "Ambient loaded: rgb=(%d,%d,%d) brightness=%d effect=%s speed=%dms",
             ambient_.color.r, ambient_.color.g, ambient_.color.b, ambient_.brightness,
             EffectName(ambient_.effect), ambient_.speed_ms);
}

void KodoLight::SaveSettings(const Ambient& ambient) {
    Settings settings(kSettingsNamespace, true);
    settings.SetInt("r", ambient.color.r);
    settings.SetInt("g", ambient.color.g);
    settings.SetInt("b", ambient.color.b);
    settings.SetInt("bright", ambient.brightness);
    settings.SetInt("speed", ambient.speed_ms);
    settings.SetString("effect", EffectName(ambient.effect));
    ESP_LOGI(TAG, "Ambient saved to NVS");
}

void KodoLight::RegisterTools() {
    auto& mcp_server = McpServer::GetInstance();

    mcp_server.AddTool(
        "self.light.set",
        "Set the ambient light of the LED ring on the robot. The ring only changes when this tool "
        "is called; it stays on the chosen light during conversation and turns off while the "
        "robot sleeps. Every call sets all parameters at once.\n"
        "Args:\n"
        "  r, g, b: color 0-255 (ignored by 'rainbow' and 'off').\n"
        "  brightness: 0-100 percent. The hardware caps the real output at 30% of full scale.\n"
        "  effect: 'solid' (steady color), 'breathe' (fade in and out, one cycle per speed_ms), "
        "'rainbow' (rotating rainbow, one turn per speed_ms) or 'off' (turn the light off).\n"
        "  speed_ms: animation period in milliseconds, 200-5000.\n"
        "  persist: true to remember the setting after reboot.\n"
        "Example: warm white steady light -> r=255,g=180,b=100,brightness=60,effect='solid'.",
        PropertyList({
            Property("r", kPropertyTypeInteger, 0, 255),
            Property("g", kPropertyTypeInteger, 0, 255),
            Property("b", kPropertyTypeInteger, 0, 255),
            Property("brightness", kPropertyTypeInteger, 0, 100),
            Property("effect", kPropertyTypeString).SetMaxLength(16),
            Property("speed_ms", kPropertyTypeInteger, 1500, 200, 5000),
            Property("persist", kPropertyTypeBoolean, false),
        }),
        [this](const PropertyList& properties) -> ToolResult {
            Ambient ambient;
            if (!ParseEffect(properties["effect"].value<std::string>(), &ambient.effect)) {
                return std::unexpected("effect must be one of: solid, breathe, rainbow, off");
            }
            ambient.color.r = static_cast<uint8_t>(properties["r"].value<int>());
            ambient.color.g = static_cast<uint8_t>(properties["g"].value<int>());
            ambient.color.b = static_cast<uint8_t>(properties["b"].value<int>());
            ambient.brightness = properties["brightness"].value<int>();
            ambient.speed_ms = properties["speed_ms"].value<int>();
            const bool persist = properties["persist"].value<bool>();

            bool sleeping;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ambient_ = ambient;
                sleeping = sleeping_;
                ShowAmbientLocked();
            }
            ESP_LOGI(TAG,
                     "Ambient set: rgb=(%d,%d,%d) brightness=%d effect=%s speed=%dms persist=%d%s",
                     ambient.color.r, ambient.color.g, ambient.color.b, ambient.brightness,
                     EffectName(ambient.effect), ambient.speed_ms, persist,
                     sleeping ? " (shown after wake)" : "");
            if (persist) {
                SaveSettings(ambient);
            }
            return true;
        });

    mcp_server.AddTool("self.light.get_state",
                       "Get the current ambient light settings of the LED ring as JSON: "
                       "{r, g, b, brightness, effect, speed_ms}.",
                       PropertyList(), [this](const PropertyList&) -> ToolResult {
                           Ambient ambient;
                           {
                               std::lock_guard<std::mutex> lock(mutex_);
                               ambient = ambient_;
                           }
                           cJSON* json = cJSON_CreateObject();
                           if (json == nullptr) {
                               return std::unexpected("Failed to allocate light state");
                           }
                           cJSON_AddNumberToObject(json, "r", ambient.color.r);
                           cJSON_AddNumberToObject(json, "g", ambient.color.g);
                           cJSON_AddNumberToObject(json, "b", ambient.color.b);
                           cJSON_AddNumberToObject(json, "brightness", ambient.brightness);
                           cJSON_AddStringToObject(json, "effect", EffectName(ambient.effect));
                           cJSON_AddNumberToObject(json, "speed_ms", ambient.speed_ms);
                           return json;
                       });
}

void KodoLight::OnStateChanged() {
    const DeviceState state = Application::GetInstance().GetDeviceState();
    std::lock_guard<std::mutex> lock(mutex_);
    if (state == kDeviceStateStarting) {
        starting_ = true;
        if (mode_ != Mode::kScroll) {
            StartModeLocked(Mode::kScroll, {kSpinnerLow, kSpinnerLow, kSpinnerHigh},
                            kSpinnerTickMs);
        }
        return;
    }
    if (starting_) {
        starting_ = false;
        ShowAmbientLocked();
        ESP_LOGI(TAG, "Boot done (state=%s): ambient shown, effect=%s",
                 DeviceStateMachine::GetStateName(state), EffectName(ambient_.effect));
        return;
    }
    // Matches KodoFaceDisplay: the sleeping face survives the good-night reply and idle,
    // any other state (wake word, button) wakes the robot.
    if (!sleeping_ || state == kDeviceStateIdle || state == kDeviceStateSpeaking ||
        state == kDeviceStateNotifying) {
        return;
    }
    sleeping_ = false;
    ShowAmbientLocked();
    ESP_LOGI(TAG, "Wake (state=%s): ambient restored, effect=%s",
             DeviceStateMachine::GetStateName(state), EffectName(ambient_.effect));
}

void KodoLight::Sleep() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sleeping_) {
        return;
    }
    sleeping_ = true;
    starting_ = false;
    ShowAmbientLocked();
    ESP_LOGI(TAG, "Sleep: ring off");
}

void KodoLight::ShowAmbientLocked() {
    if (starting_) {
        return;
    }
    if (sleeping_) {
        StartModeLocked(Mode::kFadeOut, {}, kFadeOutTickMs);
        return;
    }
    const Rgb color = {Scale(ambient_.color.r, ambient_.brightness),
                       Scale(ambient_.color.g, ambient_.brightness),
                       Scale(ambient_.color.b, ambient_.brightness)};
    switch (ambient_.effect) {
        case Effect::kSolid:
            StartModeLocked(Mode::kSolid, color, 0);
            break;
        case Effect::kBreathe:
            StartModeLocked(Mode::kBreathe, color, kBreatheTickMs, ambient_.speed_ms);
            break;
        case Effect::kRainbow:
            StartModeLocked(Mode::kRainbow, {}, kRainbowTickMs, ambient_.speed_ms);
            break;
        case Effect::kOff:
        default:
            StartModeLocked(Mode::kFadeOut, {}, kFadeOutTickMs);
            break;
    }
}

void KodoLight::StartModeLocked(Mode mode, Rgb color, int tick_ms, int period_ms) {
    esp_timer_stop(timer_);
    mode_ = mode;
    mode_color_ = color;
    mode_period_ms_ = period_ms;
    step_ = 0;
    mode_start_us_ = esp_timer_get_time();
    RenderLocked();
    if (tick_ms > 0 && mode_ != Mode::kOff) {
        esp_timer_start_periodic(timer_, tick_ms * 1000);
    }
}

void KodoLight::OnTimer() {
    std::lock_guard<std::mutex> lock(mutex_);
    step_++;
    RenderLocked();
}

void KodoLight::RenderLocked() {
    switch (mode_) {
        case Mode::kOff:
            FillLocked({});
            break;
        case Mode::kSolid:
            FillLocked(mode_color_);
            break;
        case Mode::kScroll:
            for (int i = 0; i < max_leds_; i++) {
                const int distance = (i - step_ % max_leds_ + max_leds_) % max_leds_;
                SetPixelLocked(i, distance < kSpinnerLength ? mode_color_ : Rgb{});
            }
            break;
        case Mode::kBreathe: {
            const int64_t elapsed_ms = (esp_timer_get_time() - mode_start_us_) / 1000;
            const float phase = static_cast<float>(elapsed_ms % mode_period_ms_) / mode_period_ms_;
            const int percent = static_cast<int>(
                (0.5f - 0.5f * cosf(2.0f * static_cast<float>(M_PI) * phase)) * 100.0f);
            FillLocked({Scale(mode_color_.r, percent), Scale(mode_color_.g, percent),
                        Scale(mode_color_.b, percent)});
            break;
        }
        case Mode::kRainbow: {
            const int64_t elapsed_ms = (esp_timer_get_time() - mode_start_us_) / 1000;
            const int base_hue =
                static_cast<int>(elapsed_ms % mode_period_ms_ * 360 / mode_period_ms_);
            const uint8_t value = Scale(255, ambient_.brightness);
            for (int i = 0; i < max_leds_; i++) {
                const int hue = (base_hue + i * 360 / max_leds_) % 360;
                const int rise = (hue % 60) * value / 60;
                const uint8_t up = static_cast<uint8_t>(rise);
                const uint8_t down = static_cast<uint8_t>(value - rise);
                Rgb color;
                switch (hue / 60) {
                    case 0:
                        color = {value, up, 0};
                        break;
                    case 1:
                        color = {down, value, 0};
                        break;
                    case 2:
                        color = {0, value, up};
                        break;
                    case 3:
                        color = {0, down, value};
                        break;
                    case 4:
                        color = {up, 0, value};
                        break;
                    default:
                        color = {value, 0, down};
                        break;
                }
                SetPixelLocked(i, color);
            }
            break;
        }
        case Mode::kFadeOut: {
            bool lit = false;
            for (int i = 0; i < max_leds_; i++) {
                Rgb output = {static_cast<uint8_t>(frame_[i].r / 2),
                              static_cast<uint8_t>(frame_[i].g / 2),
                              static_cast<uint8_t>(frame_[i].b / 2)};
                lit = lit || output.r != 0 || output.g != 0 || output.b != 0;
                WriteOutputLocked(i, output);
            }
            if (!lit) {
                esp_timer_stop(timer_);
                mode_ = Mode::kOff;
            }
            break;
        }
    }
    led_strip_refresh(led_strip_);
}

void KodoLight::SetPixelLocked(int index, Rgb raw) {
    WriteOutputLocked(index, {Scale(raw.r, kOutputCapPercent), Scale(raw.g, kOutputCapPercent),
                              Scale(raw.b, kOutputCapPercent)});
}

void KodoLight::WriteOutputLocked(int index, Rgb output) {
    frame_[index] = output;
    led_strip_set_pixel(led_strip_, index, output.r, output.g, output.b);
}

void KodoLight::FillLocked(Rgb raw) {
    for (int i = 0; i < max_leds_; i++) {
        SetPixelLocked(i, raw);
    }
}
