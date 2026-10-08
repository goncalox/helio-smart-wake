#include "helio_bridge.h"
#include "esphome/core/log.h"

namespace esphome::helio_bridge {
// Load old snapshot fields solely for compatibility with HA's model import.
// No wake policy, target calculation, reminder, or autonomous alarm writes remain.
void HelioBridge::setup_smart_() {
  const auto address = parent()->get_address();
  smart_pref_ = global_preferences->make_preference<smart_wake::Session>(0x48454c53U ^ uint32_t(address) ^ uint32_t(address >> 32));
  follow_pref_ = global_preferences->make_preference<smart_wake::FollowUp>(0x48454c46U ^ uint32_t(address) ^ uint32_t(address >> 32));
  if (!smart_pref_.load(&smart_session_) || smart_session_.version != 3 || !smart_session_.settings.valid()) smart_session_ = {};
  if (!follow_pref_.load(&follow_) || follow_.version != 1) follow_ = {};
  smart_status_("Home Assistant owns alarm decisions");
  smart_publish_();
}
void HelioBridge::smart_status_(const char *message) {
  if (smart_message_ == message) return;
  smart_message_ = message;
  diagnostic_text_(5, message);
  ESP_LOGI("helio_transport", "%s", message);
  if (smart_status_sensor_) smart_status_sensor_->publish_state(message);
}
void HelioBridge::smart_publish_() {
  if (smart_alarm_sensor_) publish_timestamp_(smart_alarm_sensor_, remote_.previous);
}
void HelioBridge::smart_manual_override_() { remote_manual_(); }
void HelioBridge::smart_result_(bool success) {
  if (remote_inflight_) remote_result_(success);
  smart_operation_ = false;
  smart_publish_();
}
bool HelioBridge::smart_write_allowed_() {
  return !remote_inflight_ || (clock_ && clock_->utcnow().is_valid() && remote::timely(remote_, clock_->utcnow().timestamp));
}
uint32_t HelioBridge::smart_verified_epoch_() const { return remote_inflight_ ? remote_.previous : 0; }
void HelioBridge::smart_observe_(const sleep_data::Snapshot &snapshot, uint32_t now) {
  smart_snapshot_ = snapshot;
  smart_read_at_ = now;
}
}  // namespace esphome::helio_bridge
