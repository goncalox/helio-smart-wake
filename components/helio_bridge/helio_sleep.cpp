#include "helio_bridge.h"
#include "esphome/core/log.h"
#include <cmath>

namespace esphome::helio_bridge {
static const char *const SLEEP_TAG = "helio_sleep";
void HelioBridge::sleep_status_(const char *status) {
  ESP_LOGI(SLEEP_TAG, "%s", status);
  diagnostic_text_(3, status);
  sleep_status_sensor_->publish_state(status);
}
void HelioBridge::read_sleep() {
  if (phase_ != Phase::IDLE || queued_alarm_) { sleep_status_("Bridge busy; next read will retry"); return; }
  if (clock_ == nullptr || !clock_->utcnow().is_valid()) { sleep_status_("Waiting for clock synchronization"); return; }
  sleep_status_("Reading sleep records");
  smart_read_attempt_at_ = clock_->utcnow().timestamp;
  start_(Operation::SLEEP);
}
void HelioBridge::begin_sleep_() {
  if (!sleep_available_) { fail_("Helio recorded-data service unavailable"); return; }
  const auto now = clock_->utcnow();
  if (!now.is_valid()) { fail_("Clock unavailable; sleep read stopped"); return; }
  const auto since = ESPTime::from_epoch_utc(now.timestamp - 2 * 86400);
  const std::vector<uint8_t> request{1, 0x48, uint8_t(since.year), uint8_t(since.year >> 8),
      since.month, since.day_of_month, since.hour, since.minute, 0, 0};
  ESP_LOGI(SLEEP_TAG, "Requesting sleep records since %04u-%02u-%02u %02u:%02u UTC", since.year, since.month, since.day_of_month, since.hour, since.minute);
  phase_ = Phase::SLEEP_START;
  diagnostic_sleep_.clear();
  send_(0x4b, request, sleep_encrypted_);
}
void HelioBridge::sleep_control_(const std::vector<uint8_t> &data) {
  if (data.size() < 3 || data[0] != 0x10) { fail_("Unexpected sleep transfer reply"); return; }
  if (data[1] == 1 && phase_ == Phase::SLEEP_START) {
    if (data[2] != 1) {
      ESP_LOGW(SLEEP_TAG, "Sleep request status 0x%02x", data[2]);
      fail_("Sleep records unavailable on this read"); return;
    }
    if (data.size() < 15) { fail_("Incomplete sleep transfer header"); return; }
    const uint32_t length = protocol::read32(data.data() + 3);
    if (!sleep_transfer_.start(length, clock_->utcnow().timestamp)) { fail_("Unsupported sleep transfer size"); return; }
    ESP_LOGI(SLEEP_TAG, "Sleep transfer: %u bytes; strap timestamp offset %d minutes", unsigned(length), int(int8_t(data[14])) * 15);
    if (!length) {
      phase_ = Phase::SLEEP_ACK;
      send_(0x4b, {3, 9}, sleep_encrypted_);
    } else {
      phase_ = Phase::SLEEP_DATA;
      send_(0x4b, {2}, sleep_encrypted_);
    }
    return;
  }
  if (data[1] == 2 && phase_ == Phase::SLEEP_DATA) {
    if (data[2] != 1 || (data.size() != 3 && data.size() < 7) ||
        !sleep_transfer_.complete(data.size() >= 7, data.size() >= 7 ? protocol::read32(data.data() + 3) : 0)) {
      fail_("Sleep transfer failed length, sequence or checksum validation"); return;
    }
    phase_ = Phase::SLEEP_ACK;
    // 0x09 explicitly keeps detailed records on the strap for Zepp and other readers.
    send_(0x4b, {3, 9}, sleep_encrypted_);
    return;
  }
  if (data[1] == 3 && phase_ == Phase::SLEEP_ACK) {
    if (data[2] != 1) { fail_("Sleep transfer acknowledgement rejected"); return; }
    finish_sleep_();
  }
}
void HelioBridge::sleep_bulk_(const uint8_t *data, size_t size) {
  const uint32_t offset = sleep_transfer_.bytes();
  if (!sleep_transfer_.feed(data, size)) { fail_("Sleep packets missing or out of order"); return; }
  if (diagnostic_sleep_.size() == offset && offset + size - 1 <= 594 * 32)
    diagnostic_sleep_.insert(diagnostic_sleep_.end(), data + 1, data + size);
  deadline_ = millis() + 20000;
}
void HelioBridge::publish_timestamp_(text_sensor::TextSensor *sensor, uint32_t timestamp) {
  if (!timestamp) { sensor->publish_state(""); return; }
  const auto t = ESPTime::from_epoch_utc(timestamp);
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%04u-%02u-%02uT%02u:%02u:%02uZ", t.year, t.month, t.day_of_month, t.hour, t.minute, t.second);
  sensor->publish_state(buffer);
}
void HelioBridge::update_sleep_age_() {
  const auto now = clock_->utcnow();
  sleep_age_sensor_->publish_state(sleep_through_ && now.is_valid() ? std::max<int64_t>(0, int64_t(now.timestamp) - sleep_through_) / 60.0f : NAN);
}
void HelioBridge::finish_sleep_() {
  const uint32_t now = clock_->utcnow().timestamp;
  if (diagnostic_sleep_.size() == sleep_transfer_.bytes())
    diagnostic_append_(1, diagnostic_sleep_.data(), diagnostic_sleep_.size());
  else diagnostic_text_(7, "Raw snapshot incomplete; excluded");
  diagnostic_sleep_.clear();
  publish_timestamp_(sleep_sync_sensor_, now);
  sleep_records_sensor_->publish_state(sleep_transfer_.records());
  const auto &latest = sleep_transfer_.latest();
  sleep_awake_sensor_->publish_state(latest.valid && latest.accounting_complete && !latest.is_nap ? latest.awake_minutes : NAN);
  smart_observe_(latest, now);
  ESP_LOGI(SLEEP_TAG, "Read %u sleep records: %u usable, %u empty or unsupported", sleep_transfer_.records(), sleep_transfer_.usable(), sleep_transfer_.rejected());
  if (latest.valid) {
    if (!sleep_signature_ || sleep_signature_ != latest.signature) {
      sleep_signature_ = latest.signature;
      publish_timestamp_(sleep_changed_sensor_, now);
    }
    sleep_through_ = latest.through;
    sleep_stage_sensor_->publish_state(sleep_data::stage_name(latest.stage));
    publish_timestamp_(sleep_through_sensor_, latest.through);
    publish_timestamp_(sleep_onset_sensor_, latest.onset);
    sleep_duration_sensor_->publish_state(latest.sleep_minutes ? latest.sleep_minutes :
                                         latest.accounting_complete ? latest.timeline_sleep_minutes : NAN);
    update_sleep_age_();
    char message[96];
    if (latest.through) snprintf(message, sizeof(message), "Read successful; last stage %.0f minutes old", std::max<int64_t>(0, int64_t(now) - latest.through) / 60.0);
    else snprintf(message, sizeof(message), "Read successful; no usable stage timeline");
    sleep_status_(message);
    ESP_LOGI(SLEEP_TAG, "Snapshot: onset=%u through=%u stage=%s sleep_minutes=%u signature=%08x", unsigned(latest.onset), unsigned(latest.through), sleep_data::stage_name(latest.stage), latest.sleep_minutes, unsigned(latest.signature));
    ESP_LOGI(SLEEP_TAG, "Night accounting: complete=%u awake_minutes=%u timeline_sleep_minutes=%u", latest.accounting_complete, latest.awake_minutes, latest.timeline_sleep_minutes);
  } else {
    sleep_status_(sleep_transfer_.records() ? "Records received; no usable sleep timeline" : "No sleep records returned in the last 48 hours");
    update_sleep_age_();
  }
  status_("Sleep read complete");
  if (!begin_activity_()) close_requested_ = true;
}
}  // namespace esphome::helio_bridge
