#include "helio_bridge.h"
#include "ecdh.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/components/esp32_ble_client/ble_service.h"
#include "esphome/components/esp32_ble_client/ble_characteristic.h"
#include <esp_random.h>
#include <esp_gattc_api.h>
#include <mbedtls/aes.h>
#include <cstring>

namespace esphome::helio_bridge {
static const char *const TAG = "helio_bridge";

void HelioBridge::setup() {
  diagnostic_setup_();
  diagnostic_text_(8, "Early wake policy: both sources Light or Awake");
  this->parent()->set_enabled(false);
  const auto address = this->parent()->get_address();
  alarm_pref_ = global_preferences->make_preference<alarms::Owned>(0x48454c32U ^ uint32_t(address) ^ uint32_t(address >> 32));
  if (!alarm_pref_.load(&owned_) || owned_.valid != 1 || owned_.slot > 9 || owned_.hour > 23 ||
      owned_.minute > 59 || owned_.repeat > 127) owned_ = {};
  alarm_status_(owned_.valid ? "Checking saved alarm" : "No alarm set by bridge");
  setup_smart_();
  status_("Ready for connection test");
  set_timeout("initial_test", 15000, [this]() { test_connection(); });
  sleep_status_("Waiting for first sleep read");
  set_interval("sleep_age", 60000, [this]() { update_sleep_age_(); });
  set_interval("sleep_poll", 300000, [this]() {
    if (sleep_monitoring_ && phase_ == Phase::IDLE && !queued_alarm_) read_sleep();
  });
  set_timeout("initial_sleep", 45000, [this]() {
    if (sleep_monitoring_ && phase_ == Phase::IDLE && !queued_alarm_) read_sleep();
  });
}
void HelioBridge::dump_config() { ESP_LOGCONFIG(TAG, "Helio bridge: authenticated connection and verified alarm control"); }
void HelioBridge::status_(const char *s) {
  diagnostic_text_(2, s);
  ESP_LOGI(TAG, "%s", s);
  if (status_sensor_ != nullptr) status_sensor_->publish_state(s);
}
void HelioBridge::alarm_status_(const char *s) {
  diagnostic_text_(4, s);
  ESP_LOGI(TAG, "%s", s);
  if (alarm_sensor_ != nullptr) alarm_sensor_->publish_state(s);
}
uint8_t HelioBridge::connection_indicator(uint32_t now) const {
  if (phase_ != Phase::IDLE && phase_ != Phase::CLOSING && !close_requested_) return 1;
  if (contact_failed_) return 4;
  if (!contact_seen_) return 0;
  return uint32_t(now - contact_at_) >= 900000U ? 3 : 2;
}
const char *HelioBridge::connection_label(uint32_t now) const {
  switch (connection_indicator(now)) {
    case 1: return "Checking strap";
    case 2: return "Seen recently";
    case 3: return "No recent contact";
    case 4: return "Check failed";
    default: return "Waiting for strap";
  }
}
void HelioBridge::test_connection() { start_(Operation::TEST); }
void HelioBridge::set_alarm(int hour, int minute, int repeat) {
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || repeat < 0 || repeat > 127) {
    alarm_status_("Invalid alarm time or repeat days"); return;
  }
  if (phase_ != Phase::IDLE && operation_ != Operation::SLEEP) {
    alarm_status_("Bridge busy; retry after current operation"); return;
  }
  if (!smart_dispatch_) smart_manual_override_();
  smart_operation_ = smart_dispatch_;
  requested_ = {1, 0, uint8_t(hour), uint8_t(minute), uint8_t(repeat)};
  if (phase_ != Phase::IDLE) {
    queued_alarm_ = true;
    queued_operation_ = Operation::SET_ALARM;
    alarm_status_("Alarm queued until sleep read finishes");
    return;
  }
  queued_alarm_ = false;
  alarm_status_("Connecting to save alarm");
  start_(Operation::SET_ALARM);
}
void HelioBridge::cancel_alarm() {
  if (phase_ != Phase::IDLE && operation_ != Operation::SLEEP) {
    alarm_status_("Bridge busy; retry after current operation"); return;
  }
  if (!smart_dispatch_) smart_manual_override_();
  smart_operation_ = smart_dispatch_;
  if (!owned_.valid) {
    const bool was_queued = queued_alarm_;
    queued_alarm_ = false;
    alarm_status_(was_queued ? "Queued alarm cancelled" : "No alarm set by bridge");
    return;
  }
  if (phase_ != Phase::IDLE) {
    queued_alarm_ = true;
    queued_operation_ = Operation::CANCEL_ALARM;
    alarm_status_("Cancellation queued until sleep read finishes");
    return;
  }
  queued_alarm_ = false;
  alarm_status_("Connecting to cancel alarm");
  start_(Operation::CANCEL_ALARM);
}
bool HelioBridge::start_(Operation operation) {
  if (phase_ != Phase::IDLE) { ESP_LOGW(TAG, "Helio is busy"); return false; }
  if (queued_alarm_ && (operation == Operation::TEST || operation == Operation::SLEEP)) return false;
  cancel_timeout("initial_test");
  operation_ = operation;
  phase_ = Phase::CONNECTING;
  deadline_ = millis() + 60000;
  have_session_ = false;
  write_pending_ = false;
  close_requested_ = false;
  alarm_available_ = alarm_encrypted_ = false;
  sleep_available_ = sleep_encrypted_ = false;
  sleep_deadline_ = millis() + 90000;
  outgoing_handle_ = 0;
  mtu_ = 23;
  writes_.clear();
  receiver_.reset();
  status_("Looking for Helio");
  // Wait for an advertisement so ESPHome learns the correct address type.
  this->parent()->set_enabled(true);
  return true;
}
void HelioBridge::fail_(const char *reason) {
  if (activity_phase_()) { activity_stop_(reason); return; }
  contact_failed_ = true;
  diagnostic_text_(7, reason);
  status_(reason);
  if (is_alarm_()) alarm_status_(reason);
  if (operation_ == Operation::SLEEP) sleep_status_(reason);
  if (is_alarm_()) smart_result_(false);
  close_();
}
void HelioBridge::close_() {
  phase_ = Phase::CLOSING;
  deadline_ = millis() + 4000;
  start_auth_ = false;
  cancel_timeout("verify_alarm");
  writes_.clear();
  write_pending_ = false;
  receiver_.reset();
  have_session_ = false;
  std::memset(private_key_, 0, sizeof(private_key_));
  std::memset(session_key_, 0, sizeof(session_key_));
  this->parent()->set_enabled(false);
}
void HelioBridge::loop() {
  diagnostic_tick_();
  const auto now = millis();
  if (phase_ == Phase::IDLE) {
    if (queued_alarm_) {
      queued_alarm_ = false;
      alarm_status_("Connecting for queued alarm request");
      start_(queued_operation_);
    }
    return;
  }
  if (activity_phase_() && (queued_alarm_ || int32_t(now - activity_deadline_) >= 0)) {
    activity_stop_(queued_alarm_ ? "Activity read preempted for alarm" : "Activity read timed out; sleep read retained"); return;
  }
  if (operation_ == Operation::SLEEP && phase_ != Phase::CLOSING && int32_t(now - sleep_deadline_) >= 0) {
    fail_("Sleep read timed out"); return;
  }
  if (int32_t(now - deadline_) >= 0) {
    if (phase_ == Phase::CLOSING) { phase_ = Phase::IDLE; return; }
    fail_(operation_ == Operation::SLEEP ? "Sleep read timed out" : is_alarm_() ? "Alarm operation timed out; not confirmed" : "Connection test timed out"); return;
  }
  if (start_auth_) { start_auth_ = false; begin_auth_(); }
  if (write_pending_) {
    if (int32_t(now - tx_deadline_) >= 0) fail_("Bluetooth write timed out");
    return;
  }
  if (close_requested_ && writes_.empty()) { close_requested_ = false; close_(); return; }
  if (writes_.empty() || phase_ == Phase::CLOSING) return;
  auto &write = writes_.front();
  write_pending_ = true;
  tx_deadline_ = now + 5000;
  auto result = esp_ble_gattc_write_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
      write.handle, write.data.size(), write.data.data(), ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
  if (result != ESP_OK) { ESP_LOGW(TAG, "Write error %d", result); fail_("Bluetooth write failed"); }
}
void HelioBridge::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                     esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_OPEN_EVT:
      if (param->open.status != ESP_GATT_OK) { fail_("Bluetooth connection failed"); break; }
      deadline_ = millis() + 45000;
      status_("Connected; discovering Helio services");
      break;
    case ESP_GATTC_CFG_MTU_EVT:
      if (param->cfg_mtu.status == ESP_GATT_OK) mtu_ = std::max<uint16_t>(23, param->cfg_mtu.mtu);
      break;
    case ESP_GATTC_SEARCH_CMPL_EVT: {
      if (phase_ != Phase::CONNECTING || param->search_cmpl.status != ESP_GATT_OK) {
        fail_("Bluetooth discovery failed"); break;
      }
      write_handle_ = read_handle_ = descriptor_handle_ = bulk_handle_ = bulk_descriptor_ = 0;
      uint16_t count = 0;
      auto result = esp_ble_gattc_get_attr_count(gattc_if, this->parent()->get_conn_id(),
          ESP_GATT_DB_ALL, 1, 0xffff, 0, &count);
      if (result != ESP_GATT_OK || count == 0 || count > 1024) { fail_("Cannot read Bluetooth services"); break; }
      std::vector<esp_gattc_db_elem_t> db(count);
      result = esp_ble_gattc_get_db(gattc_if, this->parent()->get_conn_id(), 1, 0xffff, db.data(), &count);
      static constexpr uint8_t base[] = {0x00,0x07,0x10,0xaf,0x09,0x00,0x18,0x21,0x12,0x35,0,0};
      if (result != ESP_GATT_OK) { fail_("Cannot read Bluetooth service details"); break; }
      for (unsigned i = 0; i < count; i++) {
        auto &entry = db[i];
        const auto *uuid = entry.uuid.uuid.uuid128;
        if (entry.type != ESP_GATT_DB_CHARACTERISTIC || entry.uuid.len != ESP_UUID_LEN_128 ||
            std::memcmp(uuid, base, sizeof(base)) != 0 || uuid[14] != 0 || uuid[15] != 0) continue;
        const auto id = protocol::read16(uuid + 12);
        if (id == 0x16) write_handle_ = entry.attribute_handle;
        if (id == 0x17) read_handle_ = entry.attribute_handle;
        if (id == 5) bulk_handle_ = entry.attribute_handle;
      }
      if (!write_handle_ || !read_handle_) { fail_("Helio protocol characteristics missing"); break; }
      esp_gattc_descr_elem_t desc{};
      esp_bt_uuid_t desc_uuid{};
      desc_uuid.len = ESP_UUID_LEN_16;
      desc_uuid.uuid.uuid16 = 0x2902;
      count = 1;
      result = esp_ble_gattc_get_descr_by_char_handle(gattc_if, this->parent()->get_conn_id(),
          read_handle_, desc_uuid, &desc, &count);
      if (result != ESP_GATT_OK || count != 1) { fail_("Helio notification descriptor missing"); break; }
      descriptor_handle_ = desc.handle;
      if (operation_ == Operation::SLEEP) {
        if (!bulk_handle_) { fail_("Helio sleep data characteristic missing"); break; }
        count = 1;
        result = esp_ble_gattc_get_descr_by_char_handle(gattc_if, this->parent()->get_conn_id(),
            bulk_handle_, desc_uuid, &desc, &count);
        if (result != ESP_GATT_OK || count != 1) { fail_("Helio sleep notification descriptor missing"); break; }
        bulk_descriptor_ = desc.handle;
      }
      phase_ = Phase::NOTIFY;
      // ESPHome writes the CCCD; wait for its completion before authenticating.
      if (this->parent()->register_for_notify(read_handle_) != ESP_OK) fail_("Cannot enable Helio notifications");
      break;
    }
    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
      if ((param->reg_for_notify.handle == read_handle_ || param->reg_for_notify.handle == bulk_handle_) && param->reg_for_notify.status != ESP_GATT_OK)
        fail_("Helio notification registration failed");
      break;
    case ESP_GATTC_WRITE_DESCR_EVT:
      if (phase_ == Phase::NOTIFY && (param->write.handle == descriptor_handle_ || param->write.handle == bulk_descriptor_)) {
        if (param->write.status != ESP_GATT_OK) { fail_("Helio notification setup failed"); break; }
        if (operation_ == Operation::SLEEP && param->write.handle == descriptor_handle_) {
          if (this->parent()->register_for_notify(bulk_handle_) != ESP_OK) fail_("Cannot enable sleep data notifications");
          break;
        }
        node_state = esp32_ble_tracker::ClientState::ESTABLISHED;
        start_auth_ = true;
      }
      break;
    case ESP_GATTC_WRITE_CHAR_EVT:
      if (write_pending_ && !writes_.empty() && param->write.handle == writes_.front().handle) {
        write_pending_ = false;
        writes_.pop_front();
        if (param->write.status != ESP_GATT_OK) fail_("Helio rejected Bluetooth write");
      }
      break;
    case ESP_GATTC_NOTIFY_EVT:
      if (param->notify.handle == read_handle_ && phase_ != Phase::IDLE && phase_ != Phase::CLOSING)
        receive_(param->notify.value, param->notify.value_len);
      else if (param->notify.handle == bulk_handle_ && phase_ == Phase::SLEEP_DATA)
        sleep_bulk_(param->notify.value, param->notify.value_len);
      else if (param->notify.handle == bulk_handle_ && phase_ == Phase::ACTIVITY_DATA)
        activity_bulk_(param->notify.value, param->notify.value_len);
      break;
    case ESP_GATTC_DISCONNECT_EVT:
      if (activity_phase_()) activity_stop_("Activity disconnected; sleep read retained");
      if (phase_ != Phase::CLOSING && phase_ != Phase::IDLE) {
        contact_failed_ = true;
        status_("Helio disconnected before operation completed");
        if (is_alarm_()) alarm_status_("Helio disconnected; alarm not confirmed");
        if (is_alarm_()) smart_result_(false);
        if (operation_ == Operation::SLEEP) sleep_status_("Helio disconnected; sleep read incomplete");
      }
      close_();
      phase_ = Phase::IDLE;
      break;
    default: break;
  }
}
bool HelioBridge::aes_(const uint8_t *key, std::vector<uint8_t> &data, bool encrypt) {
  if (data.empty() || data.size() % 16) return false;
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  int result = encrypt ? mbedtls_aes_setkey_enc(&ctx, key, 128) : mbedtls_aes_setkey_dec(&ctx, key, 128);
  for (size_t i = 0; result == 0 && i < data.size(); i += 16)
    result = mbedtls_aes_crypt_ecb(&ctx, encrypt ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT, data.data() + i, data.data() + i);
  mbedtls_aes_free(&ctx);
  return result == 0;
}
void HelioBridge::begin_auth_() {
  status_("Authenticating with Helio");
  bool success = false;
  for (unsigned i = 0; i < 8 && !success; i++) {
    esp_fill_random(private_key_, sizeof(private_key_));
    success = ecdh_generate_keys(public_key_, private_key_) != 0;
  }
  if (!success) { fail_("Authentication key exchange failed"); return; }
  std::vector<uint8_t> command{4, 2, 0, 2};
  command.insert(command.end(), public_key_, public_key_ + 48);
  phase_ = Phase::PUBLIC_KEY;
  send_(0x82, command, false);
}
void HelioBridge::send_(uint16_t endpoint, const std::vector<uint8_t> &data, bool encrypted) {
  const uint8_t handle = ++outgoing_handle_;
  std::vector<uint8_t> body(data);
  if (encrypted) {
    if (!have_session_) { fail_("Missing authenticated session"); return; }
    body.resize(((data.size() + 8 + 15) / 16) * 16, 0);
    protocol::write32(body.data() + data.size(), encrypted_sequence_++);
    protocol::write32(body.data() + data.size() + 4, protocol::crc32(body.data(), data.size() + 4));
    uint8_t key[16];
    for (unsigned i = 0; i < 16; i++) key[i] = session_key_[i] ^ handle;
    if (!aes_(key, body, true)) { fail_("Message encryption failed"); return; }
  }
  for (auto &packet : protocol::frame(endpoint, body, data.size(), mtu_, handle, encrypted))
    writes_.push_back({write_handle_, std::move(packet)});
  deadline_ = millis() + 30000;
}
void HelioBridge::receive_(const uint8_t *data, size_t size) {
  protocol::Message message;
  const int result = receiver_.feed(data, size, message);
  if (result < 0) { fail_("Malformed Helio Bluetooth message"); return; }
  if (result == 0) return;
  if (message.encrypted) {
    if (!have_session_) { fail_("Encrypted reply arrived before authentication"); return; }
    uint8_t key[16];
    for (unsigned i = 0; i < 16; i++) key[i] = session_key_[i] ^ message.handle;
    if (!aes_(key, message.data, false) ||
        protocol::crc32(message.data.data(), message.plain_size + 4) !=
        protocol::read32(message.data.data() + message.plain_size + 4)) {
      fail_("Helio reply failed integrity check"); return;
    }
  }
  // Acknowledgements go to characteristic 0x17, before the next request.
  if (message.ack) writes_.push_back({read_handle_, {4, 0, message.handle, 1, message.count}});
  message.data.resize(message.plain_size);
  payload_(message.endpoint, message.data);
}
void HelioBridge::payload_(uint16_t endpoint, const std::vector<uint8_t> &data) {
  const auto *p = data.data();
  if (endpoint == 0x82 && data.size() >= 3 && p[0] == 0x10) {
    if (p[1] == 4 && phase_ == Phase::PUBLIC_KEY) {
      if (p[2] != 1 || data.size() != 67) { fail_("Helio rejected key exchange"); return; }
      alignas(4) uint8_t remote[48], shared[48];
      std::copy_n(p + 19, 48, remote);
      for (unsigned offset : {0U, 24U}) {
        if ((remote[offset + 20] & 0xf8) || remote[offset + 21] || remote[offset + 22] || remote[offset + 23]) {
          fail_("Invalid Helio public key"); return;
        }
      }
      if (!ecdh_shared_secret(private_key_, remote, shared) ||
          std::all_of(shared, shared + 48, [](uint8_t b) { return b == 0; })) {
        fail_("Invalid Helio public key"); return;
      }
      encrypted_sequence_ = protocol::read32(shared);
      for (unsigned i = 0; i < 16; i++) session_key_[i] = shared[i + 8] ^ auth_key_[i];
      std::vector<uint8_t> proof1(p + 3, p + 19), proof2(proof1);
      if (!aes_(auth_key_, proof1, true) || !aes_(session_key_, proof2, true)) {
        fail_("Authentication encryption failed"); return;
      }
      std::vector<uint8_t> proof{5};
      proof.insert(proof.end(), proof1.begin(), proof1.end());
      proof.insert(proof.end(), proof2.begin(), proof2.end());
      have_session_ = true;
      phase_ = Phase::PROOF;
      send_(0x82, proof, false);
    } else if (p[1] == 5 && phase_ == Phase::PROOF) {
      if (p[2] != 1) { fail_(p[2] == 0x25 ? "Helio rejected the saved auth key" : "Helio rejected authentication"); return; }
      status_("Authenticated; reading available services");
      phase_ = Phase::SERVICES;
      send_(0, {3}, false);
    }
    return;
  }
  if (endpoint == 0 && phase_ == Phase::SERVICES && data.size() >= 3 && p[0] == 4) {
    const unsigned count = protocol::read16(p + 1);
    if (data.size() != 3 + count * 3) { fail_("Invalid Helio service list"); return; }
    // A valid service list proves authenticated communication on this poll.
    contact_at_ = millis(); contact_seen_ = true; contact_failed_ = false;
    bool battery_found = false, battery_encrypted = false;
    for (unsigned i = 0; i < count; i++) {
      const auto *service = p + 3 + i * 3;
      const auto id = protocol::read16(service);
      ESP_LOGD(TAG, "Service 0x%04x, encrypted=%u", id, service[2]);
      if (id == 0x29) { battery_found = true; battery_encrypted = service[2] == 1; }
      if (id == 0x4b) { sleep_available_ = true; sleep_encrypted_ = service[2] == 1; }
      if (id == 0x0f) {
        alarm_available_ = true;
        alarm_encrypted_ = service[2] == 1;
        ESP_LOGI(TAG, "Helio advertises alarm control");
      }
    }
    if (operation_ == Operation::SLEEP) {
      // Read battery on the existing connection, without blocking sleep on a reply.
      if (battery_found) send_(0x29, {3}, battery_encrypted);
      begin_sleep_(); return;
    }
    if (is_alarm_()) { request_alarms_(false); return; }
    if (!battery_found) { fail_("Authenticated; battery service unavailable"); return; }
    phase_ = Phase::BATTERY;
    send_(0x29, {3}, battery_encrypted);
    return;
  }
  if (endpoint == 0x29 && have_session_ &&
      (phase_ == Phase::BATTERY || operation_ == Operation::SLEEP) &&
      data.size() >= 4 && p[0] == 4 && p[2] <= 100) {
    battery_sensor_->publish_state(p[2]);
    ESP_LOGI(TAG, "Helio battery: %u%%", p[2]);
    if (phase_ != Phase::BATTERY) return;
    if (alarm_available_) request_alarms_(false);
    else { status_("Connection test passed; no alarm service"); close_requested_ = true; }
    return;
  }
  if (endpoint == 0x0f && !data.empty()) {
    if (p[0] == 0x0a && (phase_ == Phase::ALARMS || phase_ == Phase::ALARM_VERIFY)) {
      handle_alarms_(data); return;
    }
    if ((p[0] == 4 || p[0] == 6) && data.size() >= 2)
      ESP_LOGI(TAG, "Alarm command 0x%02x reply status 0x%02x; checking saved state", p[0], p[1]);
    return;
  }
  if (endpoint == 0x4b && operation_ == Operation::SLEEP) { if (activity_phase_()) activity_control_(data); else sleep_control_(data); return; }
  ESP_LOGD(TAG, "Ignoring endpoint 0x%04x reply (%u bytes)", endpoint, unsigned(data.size()));
}
void HelioBridge::request_alarms_(bool verify) {
  if (!alarm_available_) { fail_("Helio alarm service unavailable"); return; }
  phase_ = verify ? Phase::ALARM_VERIFY : Phase::ALARMS;
  send_(0x0f, {9}, alarm_encrypted_);
}
void HelioBridge::handle_alarms_(const std::vector<uint8_t> &data) {
  std::vector<alarms::Alarm> list;
  if (!alarms::parse(data, list)) { fail_("Invalid alarm list from Helio"); return; }
  ESP_LOGI(TAG, "Read %u alarms from Helio", unsigned(list.size()));
  for (const auto &a : list)
    ESP_LOGI(TAG, "Alarm slot %u: %02u:%02u, enabled=%u, days=0x%02x", a.slot, a.hour, a.minute, !!(a.flags & 4), a.repeat);
  // A failed update can leave the previous verified time in the same slot.
  // Recover that known ownership before retrying or cancelling, without adopting another alarm.
  if (phase_ == Phase::ALARMS && smart_operation_ && owned_.valid && smart_session_.confirmed) {
    const auto local = ESPTime::from_epoch_local(smart_session_.confirmed);
    const alarms::Owned previous{1, owned_.slot, local.hour, local.minute, 0};
    if (std::any_of(list.begin(), list.end(), [&previous](const alarms::Alarm &a) { return alarms::matches(a, previous); })) {
      owned_ = previous;
      if (!alarm_pref_.save(&owned_) || !global_preferences->sync()) { fail_("Cannot restore verified alarm ownership"); return; }
    }
  }
  const auto own = std::find_if(list.begin(), list.end(), [this](const alarms::Alarm &a) { return alarms::matches(a, owned_); });
  if (phase_ == Phase::ALARM_VERIFY) {
    if (operation_ == Operation::SET_ALARM) {
      if (own == list.end() || !(own->flags & 4)) { fail_("Alarm was not saved as requested"); return; }
      char message[96];
      snprintf(message, sizeof(message), "Saved and verified: %02u:%02u (%s)", owned_.hour, owned_.minute,
               owned_.repeat == 0 ? "once" : owned_.repeat == 127 ? "every day" : owned_.repeat == 31 ? "weekdays" : owned_.repeat == 96 ? "weekends" : "selected days");
      alarm_status_(message);
      status_("Alarm saved and verified");
    } else {
      if (std::any_of(list.begin(), list.end(), [this](const alarms::Alarm &a) { return a.slot == owned_.slot; })) {
        fail_("Alarm cancellation was not confirmed"); return;
      }
      owned_ = {};
      alarm_pref_.save(&owned_);
      global_preferences->sync();
      alarm_status_("Alarm cancelled and verified");
      status_("Alarm cancelled and verified");
    }
    smart_result_(true);
    close_requested_ = true;
    return;
  }
  if (operation_ == Operation::TEST) {
    if (own != list.end()) {
      char message[80];
      snprintf(message, sizeof(message), "Bridge alarm: %02u:%02u (%s)", own->hour, own->minute, (own->flags & 4) ? "enabled" : "disabled");
      alarm_status_(message);
    } else alarm_status_("No matching bridge alarm on strap");
    status_("Connection test passed");
    close_requested_ = true;
    return;
  }
  std::vector<uint8_t> command;
  if (operation_ == Operation::SET_ALARM) {
    if (!smart_write_allowed_()) { fail_("Smart alarm time too close or passed; existing alarm retained"); return; }
    if (smart_operation_ && smart_session_.confirmed && own == list.end()) {
      smart_session_.finished = smart_session_.manual_override = 1;
      save_smart_();
      smart_operation_ = false;
      smart_status_("Alarm changed outside bridge; smart wake paused until next evening");
      fail_("Alarm changed outside bridge; smart update stopped");
      return;
    }
    const int slot = alarms::choose_slot(list, owned_);
    if (slot < 0) { fail_("No unused alarm slot available"); return; }
    requested_.slot = slot;
    command = alarms::create(requested_);
    // Remember the selected slot before sending, so a lost reply can be recovered.
    owned_ = requested_;
    if (!alarm_pref_.save(&owned_) || !global_preferences->sync()) { fail_("Cannot remember bridge alarm slot"); return; }
    alarm_status_("Saving alarm; waiting for verification");
  } else {
    const auto slot = std::find_if(list.begin(), list.end(), [this](const alarms::Alarm &a) { return a.slot == owned_.slot; });
    if (slot == list.end()) {
      owned_ = {};
      alarm_pref_.save(&owned_);
      global_preferences->sync();
      alarm_status_("Bridge alarm is already absent");
      status_("Alarm cancellation verified");
      smart_result_(true);
      close_requested_ = true;
      return;
    }
    if (own == list.end()) { fail_("Alarm changed outside bridge; cancellation stopped"); return; }
    command = {5, 1, owned_.slot};
  }
  phase_ = Phase::ALARM_WRITE;
  send_(0x0f, command, alarm_encrypted_);
  set_timeout("verify_alarm", 1000, [this]() {
    if (phase_ == Phase::ALARM_WRITE) request_alarms_(true);
  });
}
}  // namespace esphome::helio_bridge
