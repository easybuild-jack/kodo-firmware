#include "kodo_alarm.h"

#include "application.h"
#include "assets/lang_config.h"
#include "audio/demuxer/ogg_demuxer.h"
#include "board.h"
#include "display.h"
#include "mcp_server.h"
#include "settings.h"

#include <cJSON.h>
#include <esp_log.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#define TAG "KodoAlarm"

namespace {

constexpr char kSettingsNamespace[] = "kodo_alarm";
constexpr size_t kMaxAlarms = 10;
constexpr size_t kMaxLabelBytes = 80;
// The ringtone plays once (alarm 30 s, remind 10 s); this only bounds a stuck player.
constexpr int kMaxRingSeconds = 45;
// OTA server_time sets the clock; before that it counts from 1970.
constexpr int kMinValidYear = 2025;

}  // namespace

KodoAlarm::KodoAlarm(std::function<void()> on_ring) : on_ring_(std::move(on_ring)) {
    LoadAlarms();
    RegisterTools();

    esp_timer_create_args_t timer_args = {
        .callback =
            [](void* arg) {
                auto* self = static_cast<KodoAlarm*>(arg);
                Application::GetInstance().Schedule([self]() { self->OnTick(); });
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "kodo_alarm",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer_, 1000 * 1000));

    // A wake word means the user is up: stop the ringtone before the wake reply plays.
    Application::GetInstance().OnWakeWordInvoke([this]() {
        if (ringing_) {
            ESP_LOGI(TAG, "Ringing stopped by wake word");
            FinishRing(false);
        }
    });
}

KodoAlarm::~KodoAlarm() {
    esp_timer_stop(timer_);
    esp_timer_delete(timer_);
    StopPlayer();
}

bool KodoAlarm::ParseDate(const std::string& text, int* date) {
    int year = 0, month = 0, day = 0;
    char tail = 0;
    if (std::sscanf(text.c_str(), "%4d-%2d-%2d%c", &year, &month, &day, &tail) != 3) {
        return false;
    }
    if (year < kMinValidYear || month < 1 || month > 12 || day < 1 || day > 31) {
        return false;
    }
    *date = year * 10000 + month * 100 + day;
    return true;
}

std::string KodoAlarm::FormatDate(int date) {
    if (date == 0) {
        return "";
    }
    char text[16];
    std::snprintf(text, sizeof(text), "%04d-%02d-%02d", date / 10000, date / 100 % 100,
                  date % 100);
    return text;
}

int KodoAlarm::DateOf(const tm& t) {
    return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
}

void KodoAlarm::LoadAlarms() {
    Settings settings(kSettingsNamespace);
    const std::string text = settings.GetString("list", "[]");
    const int next_id = settings.GetInt("next_id", 1);

    std::vector<Alarm> alarms;
    cJSON* root = cJSON_Parse(text.c_str());
    if (cJSON_IsArray(root)) {
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, root) {
            cJSON* id = cJSON_GetObjectItem(item, "id");
            cJSON* hour = cJSON_GetObjectItem(item, "hour");
            cJSON* minute = cJSON_GetObjectItem(item, "minute");
            cJSON* date = cJSON_GetObjectItem(item, "date");
            cJSON* days = cJSON_GetObjectItem(item, "days");
            cJSON* label = cJSON_GetObjectItem(item, "label");
            cJSON* remind = cJSON_GetObjectItem(item, "remind");
            if (!cJSON_IsNumber(id) || !cJSON_IsNumber(hour) || !cJSON_IsNumber(minute)) {
                continue;
            }
            Alarm alarm;
            alarm.id = id->valueint;
            alarm.hour = std::clamp(hour->valueint, 0, 23);
            alarm.minute = std::clamp(minute->valueint, 0, 59);
            alarm.date = cJSON_IsNumber(date) ? date->valueint : 0;
            alarm.days = cJSON_IsNumber(days) ? (days->valueint & 0x7F) : 0;
            alarm.label = cJSON_IsString(label) ? label->valuestring : "";
            alarm.remind = cJSON_IsTrue(remind);
            if (alarm.date == 0 && alarm.days == 0) {
                continue;
            }
            alarms.push_back(std::move(alarm));
        }
    }
    cJSON_Delete(root);

    std::lock_guard<std::mutex> lock(mutex_);
    alarms_ = std::move(alarms);
    next_id_ = std::max(next_id, 1);
    for (const auto& alarm : alarms_) {
        next_id_ = std::max(next_id_, alarm.id + 1);
    }
    ESP_LOGI(TAG, "Loaded %d alarm(s)", static_cast<int>(alarms_.size()));
}

void KodoAlarm::SaveAlarmsLocked() {
    cJSON* root = cJSON_CreateArray();
    for (const auto& alarm : alarms_) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", alarm.id);
        cJSON_AddNumberToObject(item, "hour", alarm.hour);
        cJSON_AddNumberToObject(item, "minute", alarm.minute);
        cJSON_AddNumberToObject(item, "date", alarm.date);
        cJSON_AddNumberToObject(item, "days", alarm.days);
        cJSON_AddStringToObject(item, "label", alarm.label.c_str());
        cJSON_AddBoolToObject(item, "remind", alarm.remind);
        cJSON_AddItemToArray(root, item);
    }
    char* text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == nullptr) {
        ESP_LOGE(TAG, "Failed to serialize alarms");
        return;
    }
    Settings settings(kSettingsNamespace, true);
    settings.SetString("list", text);
    settings.SetInt("next_id", next_id_);
    cJSON_free(text);
}

void KodoAlarm::RegisterTools() {
    auto& mcp_server = McpServer::GetInstance();

    mcp_server.AddTool(
        "self.alarm.set",
        "Set an alarm on the robot. It rings at the given local time even without a server "
        "connection.\n"
        "Args:\n"
        "  hour, minute: local time, 24-hour clock.\n"
        "  date: 'YYYY-MM-DD' for a one-shot alarm; '' when days is set.\n"
        "  days: weekday bit mask for a repeating alarm (bit0 = Sunday ... bit6 = Saturday); "
        "0 for a one-shot alarm.\n"
        "  label: what the alarm is for, may be empty.\n"
        "  kind: 'alarm' (wake-up ringtone) or 'remind' (short reminder tone).\n"
        "Returns {\"id\": n}. An alarm with the same time, date and days is replaced.",
        PropertyList({
            Property("hour", kPropertyTypeInteger, 0, 23),
            Property("minute", kPropertyTypeInteger, 0, 59),
            Property("date", kPropertyTypeString, std::string("")).SetMaxLength(10),
            Property("days", kPropertyTypeInteger, 0, 0, 127),
            Property("label", kPropertyTypeString, std::string("")).SetMaxLength(kMaxLabelBytes),
            Property("kind", kPropertyTypeString, std::string("alarm")).SetMaxLength(8),
        }),
        [this](const PropertyList& properties) -> ToolResult {
            Alarm alarm;
            alarm.hour = properties["hour"].value<int>();
            alarm.minute = properties["minute"].value<int>();
            alarm.days = properties["days"].value<int>();
            alarm.label = properties["label"].value<std::string>();
            const std::string kind = properties["kind"].value<std::string>();
            if (kind != "alarm" && kind != "remind") {
                return std::unexpected("kind must be 'alarm' or 'remind'");
            }
            alarm.remind = kind == "remind";
            const std::string date = properties["date"].value<std::string>();
            if (alarm.days == 0) {
                if (!ParseDate(date, &alarm.date)) {
                    return std::unexpected("date must be YYYY-MM-DD when days is 0");
                }
            }

            int id;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto same = std::find_if(alarms_.begin(), alarms_.end(), [&](const Alarm& a) {
                    return a.hour == alarm.hour && a.minute == alarm.minute &&
                           a.date == alarm.date && a.days == alarm.days;
                });
                if (same != alarms_.end()) {
                    same->label = alarm.label;
                    same->remind = alarm.remind;
                    id = same->id;
                } else {
                    if (alarms_.size() >= kMaxAlarms) {
                        return std::unexpected("too many alarms, cancel one first");
                    }
                    alarm.id = next_id_++;
                    id = alarm.id;
                    alarms_.push_back(alarm);
                }
                SaveAlarmsLocked();
            }
            ESP_LOGI(TAG, "Alarm set: id=%d %02d:%02d date=%s days=0x%02x kind=%s label=%s", id,
                     alarm.hour, alarm.minute, FormatDate(alarm.date).c_str(), alarm.days,
                     kind.c_str(), alarm.label.c_str());
            cJSON* json = cJSON_CreateObject();
            if (json == nullptr) {
                return std::unexpected("Failed to allocate result");
            }
            cJSON_AddNumberToObject(json, "id", id);
            return json;
        });

    mcp_server.AddTool(
        "self.alarm.list",
        "List the alarms on the robot as JSON: {\"alarms\": [{id, hour, minute, date, days, "
        "label, kind}]}. date is '' and days is non-zero for repeating alarms.",
        PropertyList(), [this](const PropertyList&) -> ToolResult {
            cJSON* json = cJSON_CreateObject();
            if (json == nullptr) {
                return std::unexpected("Failed to allocate result");
            }
            cJSON* list = cJSON_AddArrayToObject(json, "alarms");
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& alarm : alarms_) {
                cJSON* item = cJSON_CreateObject();
                cJSON_AddNumberToObject(item, "id", alarm.id);
                cJSON_AddNumberToObject(item, "hour", alarm.hour);
                cJSON_AddNumberToObject(item, "minute", alarm.minute);
                cJSON_AddStringToObject(item, "date", FormatDate(alarm.date).c_str());
                cJSON_AddNumberToObject(item, "days", alarm.days);
                cJSON_AddStringToObject(item, "label", alarm.label.c_str());
                cJSON_AddStringToObject(item, "kind", alarm.remind ? "remind" : "alarm");
                cJSON_AddItemToArray(list, item);
            }
            return json;
        });

    mcp_server.AddTool(
        "self.alarm.cancel",
        "Cancel an alarm on the robot by id. id 0 cancels every alarm.",
        PropertyList({Property("id", kPropertyTypeInteger, 0, 1000000)}),
        [this](const PropertyList& properties) -> ToolResult {
            const int id = properties["id"].value<int>();
            std::lock_guard<std::mutex> lock(mutex_);
            if (id == 0) {
                alarms_.clear();
            } else {
                auto it = std::find_if(alarms_.begin(), alarms_.end(),
                                       [id](const Alarm& a) { return a.id == id; });
                if (it == alarms_.end()) {
                    return std::unexpected("no alarm with this id");
                }
                alarms_.erase(it);
            }
            SaveAlarmsLocked();
            ESP_LOGI(TAG, "Alarm cancelled: id=%d (0 = all)", id);
            return true;
        });
}

bool KodoAlarm::Dismiss() {
    if (!ringing_) {
        return false;
    }
    Application::GetInstance().Schedule([this]() {
        if (ringing_) {
            ESP_LOGI(TAG, "Dismissed by button");
            FinishRing(true);
        }
    });
    return true;
}

void KodoAlarm::OnTick() {
    if (ringing_) {
        RingTick();
    }

    const time_t now = time(nullptr);
    tm t = {};
    localtime_r(&now, &t);
    if (t.tm_year + 1900 < kMinValidYear) {
        return;
    }
    const int64_t minute = static_cast<int64_t>(now) / 60;
    if (minute != last_minute_) {
        const bool first_tick = last_minute_ < 0;
        last_minute_ = minute;
        CheckDue(t, first_tick);
    }

    if (!ringing_ && !pending_.empty()) {
        auto& app = Application::GetInstance();
        const auto state = app.GetDeviceState();
        if (state == kDeviceStateIdle) {
            StartRing();
        } else if (state == kDeviceStateListening) {
            // A session left open (e.g. after a failed reply) would hold the alarm until the
            // server idle timeout. A reply being spoken is let finish first.
            ESP_LOGI(TAG, "Alarm pending, closing the listening session");
            app.EndListeningSession();
        }
    }
}

void KodoAlarm::CheckDue(const tm& t, bool first_tick) {
    const int today = DateOf(t);
    const int now_hm = t.tm_hour * 60 + t.tm_min;
    std::vector<Ring> due;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bool changed = false;
        for (auto it = alarms_.begin(); it != alarms_.end();) {
            const int hm = it->hour * 60 + it->minute;
            char time_text[8];
            std::snprintf(time_text, sizeof(time_text), "%02d:%02d", it->hour, it->minute);
            if (it->date != 0) {
                if (it->date == today && hm == now_hm) {
                    due.push_back({time_text, it->label, it->remind});
                    it = alarms_.erase(it);
                    changed = true;
                    continue;
                }
                // Once the clock is known, drop one-shot alarms that passed while powered off.
                if (first_tick && (it->date < today || (it->date == today && hm < now_hm))) {
                    ESP_LOGW(TAG, "Missed alarm dropped: id=%d %s %s", it->id,
                             FormatDate(it->date).c_str(), time_text);
                    it = alarms_.erase(it);
                    changed = true;
                    continue;
                }
            } else if ((it->days >> t.tm_wday) & 1 && hm == now_hm) {
                due.push_back({time_text, it->label, it->remind});
            }
            ++it;
        }
        if (changed) {
            SaveAlarmsLocked();
        }
    }
    if (due.empty()) {
        return;
    }
    // Alarms due in the same minute ring once, with their labels joined. Any wake-up alarm
    // among them makes it an alarm rather than a reminder.
    Ring ring = std::move(due.front());
    for (size_t i = 1; i < due.size(); i++) {
        ring.remind = ring.remind && due[i].remind;
        if (due[i].label.empty() || due[i].label == ring.label) {
            continue;
        }
        ring.label = ring.label.empty() ? due[i].label : ring.label + "、" + due[i].label;
    }
    ESP_LOGI(TAG, "%s due: %s %s", ring.remind ? "Reminder" : "Alarm", ring.time_text.c_str(),
             ring.label.c_str());
    pending_.push_back(std::move(ring));
}

void KodoAlarm::StartRing() {
    const Ring& ring = pending_.front();
    ringing_ = true;
    ring_seconds_ = 0;
    ESP_LOGI(TAG, "Ringing: %s %s", ring.time_text.c_str(), ring.label.c_str());
    if (on_ring_) {
        on_ring_();
    }
    auto display = Board::GetInstance().GetDisplay();
    std::string message = (ring.remind ? "提醒 " : "闹钟 ") + ring.time_text;
    if (!ring.label.empty()) {
        message += " " + ring.label;
    }
    display->SetEmotion("surprised");
    display->SetChatMessage("system", message.c_str());
    StartPlayer(ring.remind ? Lang::Sounds::OGG_REMIND_RISING : Lang::Sounds::OGG_ALARM_SUNRISE);
}

void KodoAlarm::RingTick() {
    auto& app = Application::GetInstance();
    // Anything that leaves idle (wake word, button) means the user is up and talking.
    if (app.GetDeviceState() != kDeviceStateIdle) {
        ESP_LOGI(TAG, "Ringing stopped, device left idle");
        FinishRing(false);
        return;
    }
    ring_seconds_++;
    const bool played = player_done_ && app.GetAudioService().IsPlaybackIdle();
    if (played || ring_seconds_ >= kMaxRingSeconds) {
        FinishRing(true);
    }
}

void KodoAlarm::FinishRing(bool greet) {
    ringing_ = false;
    StopPlayer();
    if (pending_.empty()) {
        return;
    }
    Ring ring = std::move(pending_.front());
    pending_.pop_front();
    if (greet) {
        Application::GetInstance().InvokeAlarm(ring.time_text, ring.label,
                                               ring.remind ? "remind" : "alarm");
    }
}

void KodoAlarm::StartPlayer(std::string_view sound) {
    StopPlayer();
    player_sound_ = sound;
    player_stop_ = false;
    player_done_ = false;
    BaseType_t ok = xTaskCreate(
        [](void* arg) {
            auto* self = static_cast<KodoAlarm*>(arg);
            self->PlayerTask();
            self->player_task_ = nullptr;
            self->player_done_ = true;
            vTaskDelete(nullptr);
        },
        "alarm_player", 4096, this, 2, &player_task_);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to start ringtone player");
        player_task_ = nullptr;
        player_done_ = true;
    }
}

void KodoAlarm::StopPlayer() {
    if (player_done_) {
        return;
    }
    player_stop_ = true;
    auto& audio = Application::GetInstance().GetAudioService();
    // Wakes a push blocked on the full decode queue and drops what is already queued.
    audio.ResetDecoder();
    for (int i = 0; i < 50 && !player_done_; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    audio.ResetDecoder();
}

void KodoAlarm::PlayerTask() {
    auto& audio = Application::GetInstance().GetAudioService();
    auto demuxer = std::make_unique<OggDemuxer>();
    demuxer->OnPacket([this, &audio](const uint8_t* data, int sample_rate,
                                     int frame_duration_ms, size_t size) {
        if (player_stop_) {
            return;
        }
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = sample_rate;
        packet->frame_duration = frame_duration_ms;
        packet->payload.resize(size);
        std::memcpy(packet->payload.data(), data, size);
        audio.PushPacketToDecodeQueue(std::move(packet), true);
    });
    demuxer->Reset();
    const auto* data = reinterpret_cast<const uint8_t*>(player_sound_.data());
    // Feed in small chunks so a stop request is seen between pages, not only per packet.
    constexpr size_t kChunk = 1024;
    for (size_t offset = 0; offset < player_sound_.size() && !player_stop_; offset += kChunk) {
        demuxer->Process(data + offset, std::min(kChunk, player_sound_.size() - offset));
    }
}
