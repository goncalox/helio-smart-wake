#include "helio_bridge.h"
#include "esphome/core/log.h"

namespace esphome::helio_bridge {
bool HelioBridge::activity_phase_() const {
  return phase_ == Phase::ACTIVITY_START || phase_ == Phase::ACTIVITY_DATA || phase_ == Phase::ACTIVITY_ACK;
}
void HelioBridge::activity_stop_(const char *reason, bool failed) {
  if (failed) read_health_.activity.failed = true;
  model_prediction_ = {}; wear_ = {};
  if (model_stage_sensor_) model_stage_sensor_->publish_state("Activity read failed");
  ESP_LOGI("helio_activity", "%s", reason);
  diagnostic_text_(10, reason);
  activity_transfer_.raw.clear();
  close_();
}
bool HelioBridge::begin_activity_() {
  const uint32_t now_ms = millis();
  const bool early_window = smart_enabled_ && !smart_session_.finished &&
      smart_wake::light_window(smart_session_, clock_->utcnow().timestamp);
  const uint32_t interval = early_window || follow_monitoring_(clock_->utcnow().timestamp) ? 60000 : 240000;
  if (queued_alarm_ || (activity_attempted_ && uint32_t(now_ms - activity_attempt_at_) < interval)) return false;
  model_prediction_ = {};
  if (model_stage_sensor_) model_stage_sensor_->publish_state("Waiting for activity read");
  activity_attempted_ = true; activity_attempt_at_ = now_ms;
  activity_deadline_ = now_ms + 15000;
  const auto since = ESPTime::from_epoch_utc(clock_->utcnow().timestamp - 30 * 60);
  activity_requested_ = since.timestamp;
  phase_ = Phase::ACTIVITY_START;
  send_(0x4b, {1, 1, uint8_t(since.year), uint8_t(since.year >> 8), since.month,
      since.day_of_month, since.hour, since.minute, 0, 0}, sleep_encrypted_);
  return true;
}
void HelioBridge::activity_control_(const std::vector<uint8_t> &data) {
  if (data.size() < 3 || data[0] != 0x10 || data[2] != 1) { activity_stop_("Activity read unavailable; sleep read retained"); return; }
  if (data[1] == 1 && phase_ == Phase::ACTIVITY_START) {
    const uint32_t now = clock_->utcnow().timestamp;
    if (data.size() >= 15) {
      char header[160];
      snprintf(header, sizeof(header), "Activity header: records=%u start=%u now=%u date=%04u-%02u-%02u %02u:%02u:%02u offset=%d",
          unsigned(protocol::read32(data.data()+3)), unsigned(activity_data::timestamp(data.data()+7)), unsigned(now),
          unsigned(data[7] | (data[8]<<8)), unsigned(data[9]), unsigned(data[10]), unsigned(data[11]), unsigned(data[12]), unsigned(data[13]), int(int8_t(data[14]))*15);
      ESP_LOGI("helio_activity", "%s", header);
      diagnostic_text_(10, header);
    }
    if (data.size() < 15 || protocol::read32(data.data()+3) > 120 ||
        !activity_transfer_.start(protocol::read32(data.data()+3) * 8,
        activity_data::timestamp(data.data()+7), now)) {
      activity_stop_("Activity header rejected: timestamp or size invalid"); return;
    }
    activity_header_.assign(data.begin()+7, data.begin()+15);
    const bool empty = protocol::read32(data.data()+3) == 0;
    phase_ = empty ? Phase::ACTIVITY_ACK : Phase::ACTIVITY_DATA;
    send_(0x4b, empty ? std::vector<uint8_t>{3, 9} : std::vector<uint8_t>{2}, sleep_encrypted_);
    return;
  }
  if (data[1] == 2 && phase_ == Phase::ACTIVITY_DATA) {
    if ((data.size() != 3 && data.size() < 7) || !activity_transfer_.complete(data.size() >= 7,
        data.size() >= 7 ? protocol::read32(data.data()+3) : 0)) {
      activity_stop_("Activity length, sequence or checksum invalid; discarded"); return;
    }
    phase_ = Phase::ACTIVITY_ACK;
    send_(0x4b, {3, 9}, sleep_encrypted_);  // Keep all records for Zepp.
    return;
  }
  if (data[1] == 3 && phase_ == Phase::ACTIVITY_ACK) {
    read_health_.activity.received(millis());
    // Version, requested UTC, actual start UTC, raw strap date, then original 8-byte rows.
    std::vector<uint8_t> payload(17);
    payload[0] = 1;
    protocol::write32(payload.data()+1, activity_requested_);
    protocol::write32(payload.data()+5, activity_transfer_.start_time);
    std::copy(activity_header_.begin(), activity_header_.end(), payload.begin()+9);
    payload.insert(payload.end(), activity_transfer_.raw.begin(), activity_transfer_.raw.end());
    diagnostic_append_(9, payload.data(), payload.size());
    const size_t count = activity_transfer_.raw.size()/8;
    const uint32_t latest = count ? activity_transfer_.start_time + (count-1)*60 : 0;
    ESP_LOGI("helio_activity", "Activity recorded: %u minutes; latest=%u; age=%d seconds (observation only)",
        unsigned(count), unsigned(latest), latest ? int(clock_->utcnow().timestamp)-int(latest) : -1);
    wear_ = smart_wake::wear_from_records(activity_transfer_.raw.data(), activity_transfer_.raw.size(),
        activity_transfer_.start_time, clock_->utcnow().timestamp);
    uint8_t observation[12] = {1, uint8_t(wear_.state), wear_.kind, wear_.heart_rate};
    protocol::write32(observation+4, wear_.sample); protocol::write32(observation+8, wear_.read_at);
    diagnostic_append_(13, observation, sizeof(observation));
    update_model_(activity_transfer_.raw.data(), activity_transfer_.raw.size(), activity_transfer_.start_time);
    activity_transfer_.raw.clear();
    close_requested_ = true;
    return;
  }
  activity_stop_("Unexpected activity response; discarded");
}
void HelioBridge::activity_bulk_(const uint8_t *data, size_t size) {
  if (!activity_transfer_.feed(data, size)) activity_stop_("Activity packets missing or out of order; discarded");
}
}  // namespace esphome::helio_bridge
