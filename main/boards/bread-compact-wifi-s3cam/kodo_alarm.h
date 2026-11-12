#ifndef KODO_ALARM_H
#define KODO_ALARM_H

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>
#include <ctime>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// Alarms set through the self.alarm.* MCP tools and kept in NVS. They ring on the device, so
// they work while no server session is open. After ringing, the robot opens a session and
// sends x_alarm so kodo-server can greet or remind the user (and snooze if asked).
//
// Two kinds: "alarm" wakes the user with the 30 s alarm_sunrise ringtone, "remind" plays the
// 10 s remind_rising tone. The clock is the local wall time from the OTA server_time (no RTC):
// alarms wait until it has been set after boot.
class KodoAlarm {
public:
    // on_ring runs on the main task when an alarm starts ringing (wake the screen and ring).
    explicit KodoAlarm(std::function<void()> on_ring);
    ~KodoAlarm();

    // BOOT button: stop a ringing alarm and greet right away. False when nothing rings.
    bool Dismiss();
    bool IsRinging() const { return ringing_; }

private:
    struct Alarm {
        int id = 0;
        int hour = 0;
        int minute = 0;
        int date = 0;  // YYYYMMDD for a one-shot alarm, 0 for a repeating one
        int days = 0;  // tm_wday bit mask (bit0 = Sunday), 0 for a one-shot alarm
        bool remind = false;
        std::string label;
    };
    struct Ring {
        std::string time_text;
        std::string label;
        bool remind = false;
    };

    // Guards alarms_ and next_id_; MCP tool callbacks and the main task both use them.
    std::mutex mutex_;
    std::vector<Alarm> alarms_;
    int next_id_ = 1;

    // Main task only.
    std::function<void()> on_ring_;
    esp_timer_handle_t timer_ = nullptr;
    int64_t last_minute_ = -1;
    std::deque<Ring> pending_;
    int ring_seconds_ = 0;
    std::atomic<bool> ringing_{false};

    // Ringtone player task: feeds the decoder one packet at a time so the main task never
    // blocks on a 30 s clip, and so the ringtone can stop at any packet.
    TaskHandle_t player_task_ = nullptr;
    std::string_view player_sound_;
    std::atomic<bool> player_stop_{false};
    std::atomic<bool> player_done_{true};

    static bool ParseDate(const std::string& text, int* date);
    static std::string FormatDate(int date);
    static int DateOf(const tm& t);

    void LoadAlarms();
    void SaveAlarmsLocked();
    void RegisterTools();

    void OnTick();
    void CheckDue(const tm& t, bool first_tick);
    void StartRing();
    void RingTick();
    void FinishRing(bool greet);

    void StartPlayer(std::string_view sound);
    void StopPlayer();
    void PlayerTask();
};

#endif  // KODO_ALARM_H
