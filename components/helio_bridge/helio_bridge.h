#pragma once
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/time/real_time_clock.h"
#include "protocol.h"
#include "alarms.h"
#include "sleep_data.h"
#include "activity_data.h"
#include "smart_wake.h"
#include "remote_control.h"
#include "adaptive_model.h"
#include "read_health.h"
#include "sleep_score_model.h"
#include <deque>
#include <nvs.h>
#include <array>

namespace esphome::helio_bridge {
class HelioBridge : public Component, public ble_client::BLEClientNode {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }
  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                          esp_ble_gattc_cb_param_t *param) override;
  void set_auth_key(const std::vector<uint8_t> &key) { std::copy_n(key.begin(), 16, auth_key_); }
  void set_status_sensor(text_sensor::TextSensor *sensor) { status_sensor_ = sensor; }
  void set_battery_sensor(sensor::Sensor *sensor) { battery_sensor_ = sensor; }
  void set_alarm_sensor(text_sensor::TextSensor *sensor) { alarm_sensor_ = sensor; }
  bool controller_owner(bool home_assistant);
  bool controller_is_remote() const {return remote_.owner;}
  void controller_alarm(int id,int epoch,int expires,int previous,bool cancel,bool follow);
  void controller_fast(int expires);
  std::string controller_status() const;
  void test_connection();
  void set_alarm(int hour, int minute, int repeat);
  void cancel_alarm();
  void read_sleep();
  // A normal BLE disconnect between polls does not mean the strap is unreachable.
  uint8_t connection_indicator(uint32_t now) const;
  const char *connection_label(uint32_t now) const;
  uint8_t read_indicator(uint32_t now) const;
  const char *read_label(uint32_t now) const;
  void download_diagnostics(int sequence);
  void diagnostic_event(const char *message);
  void set_diagnostic_status(text_sensor::TextSensor *sensor) { diagnostic_status_sensor_ = sensor; }
  void set_sleep_monitoring(bool enabled) { sleep_monitoring_ = enabled; }
  void set_clock(time::RealTimeClock *clock) { clock_ = clock; }
  void set_sleep_status(text_sensor::TextSensor *sensor) { sleep_status_sensor_ = sensor; }
  void set_sleep_stage(text_sensor::TextSensor *sensor) { sleep_stage_sensor_ = sensor; }
  void set_sleep_through(text_sensor::TextSensor *sensor) { sleep_through_sensor_ = sensor; }
  void set_sleep_onset(text_sensor::TextSensor *sensor) { sleep_onset_sensor_ = sensor; }
  void set_sleep_sync(text_sensor::TextSensor *sensor) { sleep_sync_sensor_ = sensor; }
  void set_sleep_changed(text_sensor::TextSensor *sensor) { sleep_changed_sensor_ = sensor; }
  void set_sleep_age(sensor::Sensor *sensor) { sleep_age_sensor_ = sensor; }
  void set_sleep_duration(sensor::Sensor *sensor) { sleep_duration_sensor_ = sensor; }
  void set_night_duration(sensor::Sensor *sensor) { night_duration_sensor_ = sensor; }
  void set_sleep_records(sensor::Sensor *sensor) { sleep_records_sensor_ = sensor; }
  void set_sleep_score(sensor::Sensor *sensor) { sleep_score_sensor_ = sensor; }
  void set_personal_score(sensor::Sensor *sensor) { personal_score_sensor_=sensor; }
  void set_personal_score_coverage(sensor::Sensor *sensor) { personal_score_coverage_sensor_=sensor; }
  void set_personal_score_status(text_sensor::TextSensor *sensor) { personal_score_status_sensor_=sensor; }
  void set_personal_score_details(text_sensor::TextSensor *sensor) { personal_score_details_sensor_=sensor; }
  void set_score_target(float hours);
  float displayed_personal_score() const;
  void set_learning_enabled(bool value) {learning_enabled_=value;learning_publish_();}
  void set_learning_automatic(bool value) {learning_automatic_=value;learning_publish_();}
  void set_learning_status(text_sensor::TextSensor *sensor) {learning_status_sensor_=sensor;}
  void set_model_stage(text_sensor::TextSensor *sensor) { model_stage_sensor_ = sensor; }
  void set_smart_status(text_sensor::TextSensor *sensor) { smart_status_sensor_ = sensor; }
  void set_smart_alarm(text_sensor::TextSensor *sensor) { smart_alarm_sensor_ = sensor; }
  void set_sleep_awake(sensor::Sensor *sensor) { sleep_awake_sensor_ = sensor; }
 protected:
  remote::State remote_{};
  ESPPreferenceObject remote_pref_;
  bool remote_inflight_{};
  uint32_t remote_fast_until_{};
  void setup_remote_();
  bool save_remote_();
  bool remote_fast_() const;
  void remote_result_(bool);
  void remote_manual_();
  void controller_snapshot_();
  void controller_export_(const std::vector<uint8_t>&,const char*,uint32_t);
  enum class Phase { IDLE, CONNECTING, NOTIFY, PUBLIC_KEY, PROOF, SERVICES, BATTERY, ALARMS, ALARM_WRITE, ALARM_VERIFY, SLEEP_START, SLEEP_DATA, SLEEP_ACK, ACTIVITY_START, ACTIVITY_DATA, ACTIVITY_ACK, CLOSING };
  enum class Operation { TEST, SET_ALARM, CANCEL_ALARM, SLEEP };
  struct Write { uint16_t handle; std::vector<uint8_t> data; };
  Phase phase_{Phase::IDLE};
  Operation operation_{Operation::TEST};
  uint32_t contact_at_{0};
  bool contact_seen_{false}, contact_failed_{false};
  read_health::Health read_health_;
  text_sensor::TextSensor *status_sensor_{nullptr};
  text_sensor::TextSensor *alarm_sensor_{nullptr};
  sensor::Sensor *battery_sensor_{nullptr};
  uint32_t deadline_{0}, tx_deadline_{0}, encrypted_sequence_{0};
  uint16_t write_handle_{0}, read_handle_{0}, descriptor_handle_{0}, mtu_{23};
  uint8_t outgoing_handle_{0};
  alignas(4) uint8_t private_key_[24]{}, public_key_[48]{};
  uint8_t auth_key_[16]{}, session_key_[16]{};
  bool have_session_{false}, write_pending_{false}, start_auth_{false};
  bool close_requested_{false};
  bool alarm_available_{false}, alarm_encrypted_{false};
  bool sleep_available_{false}, sleep_encrypted_{false}, sleep_monitoring_{false}, queued_alarm_{false};
  Operation queued_operation_{Operation::SET_ALARM};
  uint32_t sleep_deadline_{0}, sleep_through_{0}, sleep_signature_{0};
  std::vector<uint8_t> diagnostic_sleep_, diagnostic_pending_, diagnostic_export_;
  static constexpr size_t DIAGNOSTIC_SLOTS = 384;
  std::array<uint32_t, DIAGNOSTIC_SLOTS> diagnostic_sequences_{};
  std::array<size_t, DIAGNOSTIC_SLOTS> diagnostic_sizes_{};
  nvs_handle_t diagnostic_handle_{0};
  uint32_t diagnostic_next_{1}, diagnostic_flush_at_{0}, diagnostic_dropped_{0};
  uint32_t diagnostic_export_sequence_{0}, diagnostic_export_at_{0};
  size_t diagnostic_export_offset_{0}, diagnostic_used_{0};
  text_sensor::TextSensor *diagnostic_status_sensor_{nullptr};
  bool diagnostic_have_session_{false};
  void diagnostic_setup_();
  void diagnostic_tick_();
  void diagnostic_append_(uint8_t kind, const uint8_t *data, size_t size);
  void diagnostic_text_(uint8_t kind, const char *message);
  void diagnostic_flush_();
  bool diagnostic_evict_();
  void diagnostic_status_(const char *message);
  uint16_t bulk_handle_{0}, bulk_descriptor_{0};
  time::RealTimeClock *clock_{nullptr};
  text_sensor::TextSensor *sleep_status_sensor_{nullptr}, *sleep_stage_sensor_{nullptr}, *sleep_through_sensor_{nullptr};
  text_sensor::TextSensor *sleep_onset_sensor_{nullptr}, *sleep_sync_sensor_{nullptr}, *sleep_changed_sensor_{nullptr};
  sensor::Sensor *sleep_age_sensor_{nullptr}, *sleep_duration_sensor_{nullptr}, *sleep_records_sensor_{nullptr};
  sensor::Sensor *sleep_score_sensor_{nullptr};
  sensor::Sensor *night_duration_sensor_{nullptr};
  sleep_data::Transfer sleep_transfer_;
  activity_data::Transfer activity_transfer_;
  std::vector<uint8_t> activity_header_;
  uint32_t activity_requested_{0}, activity_attempt_at_{0}, activity_deadline_{0};
  bool activity_attempted_{false};
  bool activity_phase_() const;
  bool begin_activity_();
  void activity_stop_(const char *reason, bool failed = true);
  void activity_control_(const std::vector<uint8_t> &data);
  void activity_bulk_(const uint8_t *data, size_t size);
  sleep_score::Model *quality_{nullptr};
  nvs_handle_t quality_handle_{0};
  uint32_t quality_flush_at_{0},quality_publish_at_{0};
  bool quality_urgent_{false};
  uint32_t quality_replay_seq_{0},quality_replay_end_{0},quality_replay_at_{0};
  void score_replay_();
  sensor::Sensor *personal_score_sensor_{nullptr},*personal_score_coverage_sensor_{nullptr};
  text_sensor::TextSensor *personal_score_status_sensor_{nullptr},*personal_score_details_sensor_{nullptr};
  std::string quality_error_;
  void setup_score_(); bool save_score_(); void score_tick_(); void score_publish_();
  void score_activity_(const uint8_t *raw,size_t size,uint32_t start);
  void score_reference_(const uint8_t *raw,size_t size,uint32_t now);
  void score_audit_(const sleep_score::Night &night,uint32_t now);
  adaptive::Learner *learning_{nullptr};
  nvs_handle_t learning_handle_{0};
  bool learning_enabled_{true},learning_automatic_{true};
  uint32_t learning_flush_at_{0},learning_tick_at_{0};
  text_sensor::TextSensor *learning_status_sensor_{nullptr};
  std::string learning_message_;
  uint32_t learning_night_(uint32_t epoch) const;
  void setup_learning_(); bool save_learning_(); void learning_tick_(); void learning_publish_();
  void learning_status_(const char *message);
  void learning_observe_(const stage_model::Prediction &base);
  void learning_reference_(const uint8_t *raw,size_t size,uint32_t now);
  stage_model::Prediction model_prediction_{};
  text_sensor::TextSensor *model_stage_sensor_{nullptr};
  void update_model_(const uint8_t *raw, size_t size, uint32_t start);
  sleep_data::Snapshot smart_snapshot_{};
  smart_wake::Session smart_session_{};
  ESPPreferenceObject smart_pref_, follow_pref_;
  smart_wake::FollowUp follow_{};
  smart_wake::Wear wear_{};
  uint32_t smart_verified_epoch_() const;
  text_sensor::TextSensor *smart_status_sensor_{nullptr}, *smart_alarm_sensor_{nullptr};
  sensor::Sensor *sleep_awake_sensor_{nullptr};
  bool smart_dispatch_{false}, smart_operation_{false};
  uint32_t smart_read_at_{0}, smart_read_attempt_at_{0};
  std::string smart_message_;
  alarms::Owned owned_{}, requested_{};
  ESPPreferenceObject alarm_pref_;
  std::deque<Write> writes_;
  protocol::Receiver receiver_;
  void status_(const char *status);
  bool start_(Operation operation);
  void alarm_status_(const char *status);
  void request_alarms_(bool verify);
  void handle_alarms_(const std::vector<uint8_t> &data);
  bool is_alarm_() const { return operation_ == Operation::SET_ALARM || operation_ == Operation::CANCEL_ALARM; }
  void sleep_status_(const char *status);
  void begin_sleep_();
  void sleep_control_(const std::vector<uint8_t> &data);
  void sleep_bulk_(const uint8_t *data, size_t size);
  void finish_sleep_();
  void update_sleep_age_();
  void setup_smart_();
  void smart_status_(const char *message);
  void smart_manual_override_();
  void smart_result_(bool success);
  void smart_observe_(const sleep_data::Snapshot &snapshot, uint32_t now);
  void smart_publish_();
  bool smart_write_allowed_();
  void publish_timestamp_(text_sensor::TextSensor *sensor, uint32_t timestamp);
  void fail_(const char *reason);
  void close_();
  void begin_auth_();
  void send_(uint16_t endpoint, const std::vector<uint8_t> &data, bool encrypted);
  void receive_(const uint8_t *data, size_t size);
  void payload_(uint16_t endpoint, const std::vector<uint8_t> &data);
  bool aes_(const uint8_t *key, std::vector<uint8_t> &data, bool encrypt);
};
}  // namespace esphome::helio_bridge
