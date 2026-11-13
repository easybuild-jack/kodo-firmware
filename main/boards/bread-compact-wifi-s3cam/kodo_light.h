#ifndef KODO_LIGHT_H
#define KODO_LIGHT_H

#include "led/led.h"

#include <driver/gpio.h>
#include <esp_timer.h>
#include <led_strip.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// WS2812 ring that shows the user's ambient light, set through the self.light.* MCP tools.
// Apart from the boot spinner, nothing but those tools changes it: device states and sleep
// leave the ring as the user set it.
class KodoLight : public Led {
public:
    KodoLight(gpio_num_t gpio, uint16_t max_leds);
    ~KodoLight() override;

    void OnStateChanged() override;
    // Offline 开灯/关灯: on restores the last color as a steady light; both are remembered.
    void SetPower(bool on);

private:
    enum class Effect { kOff, kSolid, kBreathe, kRainbow };
    enum class Mode { kOff, kFadeOut, kSolid, kScroll, kBreathe, kRainbow };

    struct Rgb {
        uint8_t r = 0, g = 0, b = 0;
    };

    struct Ambient {
        Rgb color = {255, 255, 255};
        int brightness = 50;
        Effect effect = Effect::kOff;
        int speed_ms = 1500;
    };

    // Guards every member below; taken by the main task, MCP callbacks and the effect timer.
    std::mutex mutex_;
    led_strip_handle_t led_strip_ = nullptr;
    esp_timer_handle_t timer_ = nullptr;
    int max_leds_ = 0;
    std::vector<Rgb> frame_;
    Ambient ambient_;
    // Boot phase: the spinner runs until the first state after kDeviceStateStarting.
    bool starting_ = true;

    Mode mode_ = Mode::kOff;
    Rgb mode_color_;
    int mode_period_ms_ = 0;
    int step_ = 0;
    int64_t mode_start_us_ = 0;

    static const char* EffectName(Effect effect);
    static bool ParseEffect(const std::string& name, Effect* effect);

    void LoadSettings();
    void SaveSettings(const Ambient& ambient);
    void RegisterTools();

    void ShowAmbientLocked();
    void StartModeLocked(Mode mode, Rgb color, int tick_ms, int period_ms = 0);
    void RenderLocked();
    void OnTimer();

    void SetPixelLocked(int index, Rgb raw);
    void WriteOutputLocked(int index, Rgb output);
    void FillLocked(Rgb raw);
};

#endif  // KODO_LIGHT_H
