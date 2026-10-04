#include "helio_bridge.h"
#include "esphome/core/log.h"
#include <cmath>

namespace esphome::helio_bridge {
namespace {
uint32_t local_calendar(uint32_t epoch, int hour, int minute, int days = 0) {
  auto date = ESPTime::from_epoch_local(epoch);
  date.hour = hour;
  date.minute = minute;
  date.second = 0;
  date.recalc_timestamp_utc(false);
  date = ESPTime::from_epoch_utc(date.timestamp + days * 86400);
  date.recalc_timestamp_local();
  return date.timestamp;
}
}
void HelioBridge::setup_smart_() {
  const auto address = parent()->get_address();
  smart_pref_ = global_preferences->make_preference<smart_wake::Session>(0x48454c53U ^ uint32_t(address) ^ uint32_t(address >> 32));
  const bool loaded = smart_pref_.load(&smart_session_);
  const bool migrate = loaded && (smart_session_.version == 1 || smart_session_.version == 2);
  if (migrate) {
    if (smart_session_.version == 1) smart_session_.awake_minutes = 0;
    smart_session_.version = 3;
    // Persist the migration marker until the clock can convert the old deadline.
    smart_session_.settings.migration_pending = 1;
    smart_session_.settings.reserved = 0;
  }
  if (!loaded || smart_session_.version != 3 || !smart_session_.settings.valid() || smart_session_.awake_minutes > 1440 ||
      (smart_session_.session_end && (smart_session_.night_start >= smart_session_.session_end ||
       smart_session_.session_end - smart_session_.night_start > 26 * 3600))) smart_session_ = {};
  if (migrate) save_smart_();
  smart_status_("Waiting for smart wake settings");
  smart_publish_();
}
bool HelioBridge::save_smart_() {
  if (smart_pref_.save(&smart_session_) && global_preferences->sync()) return true;
  smart_status_("Cannot save smart wake state; alarm update stopped");
  return false;
}
void HelioBridge::smart_status_(const char *message) {
  if (smart_message_ == message) return;
  smart_message_ = message;
  diagnostic_text_(5, message);
  ESP_LOGI("helio_smart", "%s", message);
  if (smart_status_sensor_) smart_status_sensor_->publish_state(message);
}
void HelioBridge::smart_publish_() {
  if (!diagnostic_have_session_ || memcmp(&diagnostic_last_session_, &smart_session_, sizeof(smart_session_))) {
    diagnostic_last_session_ = smart_session_;
    diagnostic_have_session_ = true;
    diagnostic_append_(6, reinterpret_cast<const uint8_t *>(&smart_session_), sizeof(smart_session_));
  }
  if (smart_alarm_sensor_) publish_timestamp_(smart_alarm_sensor_, smart_session_.confirmed);
  if (smart_target_sensor_) publish_timestamp_(smart_target_sensor_, smart_session_.session_end ? smart_wake::target(smart_session_) : 0);
  if (smart_awake_sensor_) smart_awake_sensor_->publish_state(smart_session_.awake_minutes);
}
void HelioBridge::smart_manual_override_() {
  if (!smart_session_.session_end) return;
  smart_session_.finished = 1;
  smart_session_.manual_override = 1;
  smart_session_.cancel_pending = 0;
  smart_session_.confirmed = smart_session_.attempted = 0;
  smart_session_.awake_minutes = 0;
  smart_operation_ = false;
  save_smart_();
  smart_publish_();
  smart_status_("Manual alarm control; smart wake paused until next evening");
}
void HelioBridge::smart_result_(bool success) {
  if (!smart_operation_) return;
  smart_operation_ = false;
  const uint32_t now = clock_->utcnow().timestamp;
  smart_retry_at_ = now + 60;
  if (!success) {
    smart_status_(smart_session_.cancel_pending ? "Cancellation unconfirmed; saved alarm may remain; retrying" : "Alarm update unconfirmed; retrying while time permits");
    return;
  }
  if (operation_ == Operation::CANCEL_ALARM) {
    smart_session_.cancel_pending = 0;
    smart_session_.confirmed = smart_session_.attempted = 0;
    smart_status_(smart_enabled_ ? "Previous smart alarm removed; sleep-based scheduling active" : "Smart wake off; alarm cancellation verified");
  } else {
    smart_session_.confirmed = smart_session_.attempted;
    smart_status_(smart_session_.early_selected ? "Both report light; earlier alarm saved and verified" :
                  smart_session_.onset ? "Sleep-based alarm saved and verified" : "Sleep-based alarm saved and verified");
  }
  save_smart_();
  smart_publish_();
}
bool HelioBridge::smart_write_allowed_() {
  if (!smart_operation_ || operation_ != Operation::SET_ALARM) return true;
  const auto now = clock_->utcnow();
  return now.is_valid() && smart_session_.attempted >= uint64_t(now.timestamp) + 30 &&
         (!smart_session_.confirmed || smart_session_.confirmed >= uint64_t(now.timestamp) + 30) &&
         smart_session_.attempted < uint64_t(now.timestamp) + 24 * 3600 - 60;
}
void HelioBridge::smart_observe_(const sleep_data::Snapshot &snapshot, uint32_t now) {
  smart_snapshot_ = snapshot;
  smart_read_at_ = now;
  if (!smart_enabled_ || !smart_session_.session_end || smart_session_.finished || smart_session_.early_selected) return;
  if (smart_session_.attempted && smart_session_.attempted != smart_session_.confirmed) return;
  if (!smart_wake::usable_onset(smart_session_, snapshot, now)) {
    smart_candidate_ = smart_candidate_since_ = 0;
    return;
  }
  if (smart_session_.onset != snapshot.onset) {
    if (smart_candidate_ != snapshot.onset) {
      smart_candidate_ = snapshot.onset;
      smart_candidate_since_ = now;
      return;
    }
    // Require two matching onset observations at least five minutes apart.
    if (now < smart_candidate_since_ + 300) return;
  }
  auto proposed = smart_session_;
  proposed.onset = snapshot.onset;
  // Replace the total from this complete record; never add repeated snapshots together.
  proposed.awake_minutes = snapshot.awake_minutes;
  if (proposed.onset == smart_session_.onset && proposed.awake_minutes == smart_session_.awake_minutes) return;
  if (smart_wake::target(proposed) != smart_wake::target(smart_session_) &&
      !smart_wake::writable(smart_wake::target(proposed), now)) return;
  smart_session_ = proposed;
  save_smart_();
  smart_publish_();
}
void HelioBridge::tick_smart(float hours, int early_minutes) {
  if (clock_ == nullptr || !clock_->utcnow().is_valid()) {
    smart_status_(smart_enabled_ ? "Waiting for clock synchronization" : "Smart wake off");
    return;
  }
  const uint32_t now = clock_->utcnow().timestamp;
  const auto matches_time = [this](uint32_t epoch) {
    const auto local = ESPTime::from_epoch_local(epoch);
    return epoch && owned_.valid && owned_.repeat == 0 && owned_.hour == local.hour && owned_.minute == local.minute;
  };
  if (smart_session_.settings.migration_pending) {
    const uint32_t old_deadline = smart_session_.session_end;
    if (old_deadline && now >= old_deadline) smart_session_.finished = 1;
    smart_session_.session_end = smart_session_.night_start ? local_calendar(smart_session_.night_start, 18, 0, 1) : 0;
    smart_session_.cancel_pending = !smart_session_.manual_override &&
        (matches_time(smart_session_.attempted) || matches_time(smart_session_.confirmed));
    if (!smart_session_.cancel_pending) smart_session_.confirmed = smart_session_.attempted = 0;
    smart_session_.settings.migration_pending = 0;
    if (!save_smart_()) return;
    smart_publish_();
    diagnostic_text_(8, "Hard wake deadline removed; no automatic clock-time fallback");
  }
  if (smart_enabled_ && !smart_session_.finished && smart_session_.confirmed &&
      (smart_message_ == "Waiting for clock synchronization" || smart_message_ == "Waiting for smart wake settings")) {
    smart_status_("Smart wake active; saved alarm retained");
  }
  if (!smart_enabled_ && smart_session_.session_end && !smart_session_.cancel_pending && !smart_session_.manual_override &&
      (!smart_session_.finished || smart_session_.confirmed > now || smart_session_.attempted > now)) {
    smart_session_.finished = 2;
    smart_session_.cancel_pending = smart_session_.attempted || smart_session_.confirmed;
    if (!save_smart_()) return;
  }
  if (smart_session_.cancel_pending) {
    if (phase_ != Phase::IDLE || queued_alarm_ || now < smart_retry_at_) return;
    if (!matches_time(smart_session_.attempted) && !matches_time(smart_session_.confirmed)) {
      smart_session_.cancel_pending = 0;
      smart_session_.confirmed = smart_session_.attempted = 0;
      save_smart_();
      smart_publish_();
      smart_status_(smart_enabled_ ? "No previous smart alarm to remove; sleep-based scheduling active" : "Smart wake off; no matching smart alarm to cancel");
      return;
    }
    smart_dispatch_ = true;
    cancel_alarm();
    smart_dispatch_ = false;
    smart_retry_at_ = now + 60;
    return;
  }
  if (!smart_enabled_) { smart_status_("Smart wake off"); return; }
  // A cancelled, disabled session can be re-enabled before its wake time passed.
  if (smart_session_.finished == 2 && !smart_session_.manual_override && !smart_session_.cancel_pending)
    smart_session_ = {};
  if (!std::isfinite(hours) || hours < 6 || hours > 10 || early_minutes < 0 || early_minutes > smart_wake::MAX_EARLY_MINUTES) {
    smart_status_("Invalid settings: target 6-10h, window 0-60min"); return;
  }
  if (smart_session_.session_end && !smart_session_.finished && smart_wake::elapsed(smart_session_, now)) {
    smart_session_.finished = 1;
    if (!save_smart_()) return;
  }
  if (smart_session_.finished && now < local_calendar(smart_session_.night_start, 18, 0, 1)) {
    smart_status_(smart_session_.manual_override ? "Manual alarm control; smart wake paused until next evening" :
                  smart_session_.confirmed > now ? "Earlier write unconfirmed; last verified alarm remains scheduled" :
                  smart_session_.confirmed ? "Scheduled wake time passed; next night arms at 18:00" : "Wake time passed without a verified alarm; next night arms at 18:00");
    return;
  }
  if (!smart_session_.session_end || smart_session_.finished) {
    if (phase_ != Phase::IDLE || queued_alarm_) return;
    auto night_start = local_calendar(now, 18, 0);
    if (night_start > now) night_start = local_calendar(now, 18, 0, -1);
    smart_session_ = {};
    smart_session_.night_start = night_start;
    // This selects a dated sleep session; it never caps or creates a wake alarm.
    smart_session_.session_end = local_calendar(night_start, 18, 0, 1);
    smart_session_.settings = {uint16_t(std::lround(hours * 60)), uint16_t(early_minutes), 0, 0};
    smart_candidate_ = smart_candidate_since_ = smart_retry_at_ = 0;
    if (!save_smart_()) return;
    smart_publish_();
  }
  if (phase_ != Phase::IDLE || queued_alarm_) return;
  const auto desired = smart_wake::desired_alarm(smart_session_, smart_snapshot_, now, smart_read_at_, model_prediction_);
  if (smart_wake::light_window(smart_session_, now)) log_early_gate_(now, desired);
  if (desired && desired != smart_session_.confirmed && now >= smart_retry_at_) {
    if (!smart_wake::writable(desired, now)) {
      smart_status_(smart_session_.confirmed ? "Too close to change alarm; retaining last verified time" : "Too close to safely save alarm; no verified alarm");
      return;
    }
    smart_session_.early_selected = desired < smart_wake::target(smart_session_);
    smart_session_.attempted = desired;
    if (!save_smart_()) return;
    const auto local = ESPTime::from_epoch_local(desired);
    smart_status_(smart_session_.early_selected ? "Both report light; saving earlier alarm" : "Saving smart wake alarm");
    smart_dispatch_ = true;
    set_alarm(local.hour, local.minute, 0);
    smart_dispatch_ = false;
    smart_retry_at_ = now + 60;
    return;
  }
  if (!smart_session_.onset) smart_status_("Waiting for stable sleep onset; no automatic fallback alarm");
  const auto wake = smart_session_.confirmed ? smart_session_.confirmed : smart_wake::target(smart_session_);
  const bool near_wake = wake > now && wake - now <= 15 * 60;
  if (sleep_monitoring_ && (near_wake || smart_wake::light_window(smart_session_, now)) && now >= smart_read_attempt_at_ + 60) read_sleep();
}
}  // namespace esphome::helio_bridge
