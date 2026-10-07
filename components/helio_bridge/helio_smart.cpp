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
  follow_pref_ = global_preferences->make_preference<smart_wake::FollowUp>(0x48454c46U ^ uint32_t(address) ^ uint32_t(address >> 32));
  if (!follow_pref_.load(&follow_) || follow_.version != 1 || follow_.stopped > 1 ||
      follow_.cancel_pending > 1 || follow_.uncertain > 1 ||
      (follow_.primary && (!follow_.night_start || (!follow_.stopped && follow_.confirmed < follow_.primary) || follow_.attempted < follow_.confirmed))) follow_ = {};
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
  if (smart_alarm_sensor_) publish_timestamp_(smart_alarm_sensor_,
      follow_.night_start == smart_session_.night_start && follow_.primary ? follow_.confirmed : smart_session_.confirmed);
  if (smart_target_sensor_) publish_timestamp_(smart_target_sensor_, smart_session_.session_end ? smart_wake::target(smart_session_) : 0);
  if (smart_awake_sensor_) smart_awake_sensor_->publish_state(smart_session_.awake_minutes);
}
void HelioBridge::smart_manual_override_() {
  remote_manual_();
  follow_.stopped = 1; follow_.cancel_pending = 0;
  follow_operation_ = false;
  save_follow_();
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
  if(remote_inflight_){remote_result_(success);return;}
  if (!smart_operation_) return;
  smart_operation_ = false;
  smart_new_attempt_ = false;
  const uint32_t now = clock_->utcnow().timestamp;
  if (follow_operation_) {
    follow_operation_ = false;
    smart_retry_at_ = now + 10;
    if (!success) {
      smart_status_(follow_.cancel_pending ? "Follow-up cancellation unconfirmed; retrying" : "Follow-up update unconfirmed; retrying while time permits");
      return;
    }
    if (operation_ == Operation::CANCEL_ALARM) {
      follow_.cancel_pending = 0;
      follow_.confirmed = follow_.attempted = 0;
      if (smart_session_.finished == 2) {
        smart_session_.confirmed = smart_session_.attempted = 0;
        smart_session_.cancel_pending = 0; save_smart_();
      }
      smart_status_("Strap removed or smart wake off; follow-up cancellation verified");
    } else {
      follow_.confirmed = follow_.attempted;
      follow_.uncertain = 0;
      smart_status_("Strap still worn; five-minute follow-up saved and verified");
    }
    save_follow_(); smart_publish_();
    return;
  }
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
    // A verified write may be acknowledged just after its minute has passed.
    if (!smart_session_.manual_override && smart_session_.finished != 2 &&
        uint64_t(now) <= uint64_t(smart_session_.confirmed) + 120) {
      follow_ = {};
      follow_.night_start = smart_session_.night_start;
      follow_.primary = follow_.confirmed = follow_.attempted = smart_session_.confirmed;
      save_follow_();
    }
    smart_status_(smart_session_.early_selected ? "Both report light or awake; earlier alarm saved and verified" :
                  smart_session_.onset ? "Sleep-based alarm saved and verified" : "Sleep-based alarm saved and verified");
  }
  save_smart_();
  smart_publish_();
}
bool HelioBridge::smart_write_allowed_() {
  if(remote_inflight_)return clock_ && clock_->utcnow().is_valid() && remote::timely(remote_,clock_->utcnow().timestamp);
  if (!smart_operation_ || operation_ != Operation::SET_ALARM) return true;
  const auto now = clock_->utcnow();
  const uint32_t candidate = follow_operation_ ? follow_.attempted : smart_session_.attempted;
  return now.is_valid() && candidate >= uint64_t(now.timestamp) + smart_wake::SEND_MARGIN &&
         (follow_operation_ || !smart_session_.confirmed || smart_session_.confirmed >= uint64_t(now.timestamp) + smart_wake::SEND_MARGIN) &&
         candidate < uint64_t(now.timestamp) + 24 * 3600 - 60;
}
uint32_t HelioBridge::smart_verified_epoch_() const {
  return remote_inflight_ ? remote_.previous : follow_operation_ ? follow_.confirmed : smart_session_.confirmed;
}
bool HelioBridge::save_follow_() {
  if (!follow_pref_.save(&follow_) || !global_preferences->sync()) {
    smart_status_("Cannot save follow-up state; alarm update stopped");
    return false;
  }
  diagnostic_append_(14, reinterpret_cast<const uint8_t *>(&follow_), sizeof(follow_));
  return true;
}
void HelioBridge::smart_unsent_() {
  if(remote_inflight_)return;
  // Only discard a newly selected attempt. A retry/reboot may follow an uncertain write.
  if (!smart_operation_ || !smart_new_attempt_) return;
  if (follow_operation_) {
    follow_.attempted = follow_.confirmed; follow_.uncertain = 0; save_follow_();
  } else {
    smart_session_.attempted = smart_session_.confirmed;
    smart_session_.early_selected = smart_session_.confirmed && smart_session_.confirmed < smart_wake::target(smart_session_);
    save_smart_();
  }
}
bool HelioBridge::follow_monitoring_(uint32_t now) const {
  auto next_night = local_calendar(follow_.primary, 18, 0);
  if (next_night <= follow_.primary) next_night = local_calendar(follow_.primary, 18, 0, 1);
  return smart_enabled_ && !smart_session_.manual_override && follow_.primary && !follow_.stopped &&
      follow_.night_start == smart_session_.night_start && now >= follow_.primary && now < next_night;
}
void HelioBridge::stop_follow_() {
  if (!smart_enabled_) { smart_session_.finished = 2; save_smart_(); }
  follow_.stopped = 1;
  follow_.cancel_pending = follow_.attempted > clock_->utcnow().timestamp || follow_.confirmed > clock_->utcnow().timestamp;
  save_follow_();
}
bool HelioBridge::tick_follow_(uint32_t now) {
  if (!follow_.primary || follow_.night_start != smart_session_.night_start) return false;
  const auto matches = [this](uint32_t epoch) {
    const auto local = ESPTime::from_epoch_local(epoch);
    return epoch && owned_.valid && owned_.repeat == 0 && owned_.hour == local.hour && owned_.minute == local.minute;
  };
  if (follow_.cancel_pending) {
    if (phase_ != Phase::IDLE || queued_alarm_ || now < smart_retry_at_) return true;
    if (!matches(follow_.attempted) && !matches(follow_.confirmed)) {
      follow_.cancel_pending = 0; save_follow_();
      smart_status_("Follow-ups stopped; no matching bridge alarm to cancel");
      return true;
    }
    follow_operation_ = true; smart_dispatch_ = true;
    cancel_alarm(); smart_dispatch_ = false; smart_retry_at_ = now + 10;
    return true;
  }
  if (!follow_monitoring_(now)) return false;
  // Both detection and follow-up require a completed minute from after the first alarm.
  if (wear_.fresh_after(follow_.primary, now) && wear_.state == smart_wake::WearState::REMOVED) {
    stop_follow_(); smart_status_("Strap removed; follow-ups stopped"); return true;
  }
  if (phase_ != Phase::IDLE || queued_alarm_) return true;
  const auto desired = smart_wake::follow_alarm(follow_, wear_, now);
  if (follow_.attempted != follow_.confirmed && follow_.attempted <= now) {
    follow_.stopped = 1; save_follow_();
    smart_status_("Follow-up write unverified at its time; further alarms stopped");
    return true;
  }
  if (desired && desired != follow_.confirmed && now >= smart_retry_at_) {
    auto next_night = local_calendar(follow_.primary, 18, 0);
    if (next_night <= follow_.primary) next_night = local_calendar(follow_.primary, 18, 0, 1);
    if (desired >= next_night) {
      follow_.stopped = 1; save_follow_();
      smart_status_("Follow-up sequence ended; next dated night arms at 18:00"); return true;
    }
    if (smart_wake::writable(desired, now)) {
      smart_new_attempt_ = follow_.attempted == follow_.confirmed;
      follow_.attempted = desired; follow_.uncertain = 1;
      if (!save_follow_()) return true;
      const auto local = ESPTime::from_epoch_local(desired);
      follow_operation_ = true; smart_dispatch_ = true;
      smart_status_("Strap still worn after alarm; saving five-minute follow-up");
      set_alarm(local.hour, local.minute, 0); smart_dispatch_ = false;
      smart_retry_at_ = now + 10;
      return true;
    }
  }
  smart_status_(follow_.confirmed > now ? "Follow-up scheduled; monitoring for strap removal" :
      "Wake time passed; waiting for fresh strap-worn data");
  if (sleep_monitoring_ && now >= smart_read_attempt_at_ + 60) read_sleep();
  return true;
}
void HelioBridge::smart_observe_(const sleep_data::Snapshot &snapshot, uint32_t now) {
  smart_snapshot_ = snapshot;
  smart_read_at_ = now;
  if (remote_.owner || !smart_enabled_ || !smart_session_.session_end || smart_session_.finished || smart_session_.early_selected) return;
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
  if(remote_.owner){smart_status_("Home Assistant owns alarm decisions");return;}
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
  if (!smart_enabled_ && follow_.primary && !follow_.stopped) stop_follow_();
  if (follow_.cancel_pending && tick_follow_(now)) return;
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
  if (!follow_.primary && !smart_session_.finished && !smart_session_.manual_override && smart_session_.confirmed > now) {
    follow_ = {}; follow_.night_start = smart_session_.night_start;
    follow_.primary = follow_.confirmed = follow_.attempted = smart_session_.confirmed;
    if (!save_follow_()) return;
  }
  if (smart_session_.session_end && !smart_session_.finished && smart_wake::elapsed(smart_session_, now)) {
    smart_session_.finished = 1;
    if (!save_smart_()) return;
  }
  if (smart_session_.finished && tick_follow_(now)) return;
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
    follow_ = {}; wear_ = {};
    if (!save_follow_()) return;
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
    smart_new_attempt_ = smart_session_.attempted == smart_session_.confirmed;
    smart_session_.early_selected = desired < smart_wake::target(smart_session_);
    smart_session_.attempted = desired;
    if (!save_smart_()) return;
    const auto local = ESPTime::from_epoch_local(desired);
    smart_status_(smart_session_.early_selected ? "Both report light or awake; saving earlier alarm" : "Saving smart wake alarm");
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
