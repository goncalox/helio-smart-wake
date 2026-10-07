#include "helio_bridge.h"
#include "diagnostic_codec.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include <cstring>

namespace esphome::helio_bridge {
static constexpr size_t MAX_PENDING = 24576, STORAGE_BUDGET = 256 * 1024;
static void key_for(uint32_t slot, char *key) { snprintf(key, 12, "b%03u", unsigned(slot)); }
void HelioBridge::diagnostic_status_(const char *message) {
  char s[160];
  uint32_t oldest = diagnostic_next_;
  for (auto seq : diagnostic_sequences_) if (seq && seq < oldest) oldest = seq;
  snprintf(s, sizeof(s), "%s; batches %u-%u; %u bytes; dropped %u", message,
      unsigned(oldest), unsigned(diagnostic_next_ - 1), unsigned(diagnostic_used_), unsigned(diagnostic_dropped_));
  ESP_LOGI("helio_log", "%s", s);
  if (diagnostic_status_sensor_) diagnostic_status_sensor_->publish_state(s);
}
void HelioBridge::diagnostic_setup_() {
  if (nvs_open("helio_log_v2", NVS_READWRITE, &diagnostic_handle_) != ESP_OK) {
    diagnostic_handle_ = 0; diagnostic_status_("Storage unavailable"); return;
  }
  // Rebuild the index from committed blobs: no frequently rewritten head pointer.
  for (size_t i = 0; i < DIAGNOSTIC_SLOTS; i++) {
    char key[12]; key_for(i, key); size_t n = 0;
    if (nvs_get_blob(diagnostic_handle_, key, nullptr, &n) != ESP_OK) continue;
    if (n < 16 || n > MAX_PENDING * 2 + 16) { nvs_erase_key(diagnostic_handle_, key); continue; }
    std::vector<uint8_t> blob(n), raw;
    if (nvs_get_blob(diagnostic_handle_, key, blob.data(), &n) != ESP_OK) continue;
    uint32_t seq = protocol::read32(blob.data() + 4), length = protocol::read32(blob.data() + 8);
    if (memcmp(blob.data(), "HLG2", 4) || !seq || seq % DIAGNOSTIC_SLOTS != i || length > MAX_PENDING ||
        !diagnostic::decode(blob.data() + 16, n - 16, length, raw) ||
        diagnostic::crc(raw.data(), raw.size()) != protocol::read32(blob.data() + 12)) {
      nvs_erase_key(diagnostic_handle_, key); ++diagnostic_dropped_; continue;
    }
    diagnostic_sequences_[i] = seq; diagnostic_sizes_[i] = n + 96;
    diagnostic_used_ += n + 96; diagnostic_next_ = std::max(diagnostic_next_, seq + 1);
  }
  nvs_commit(diagnostic_handle_);
  diagnostic_flush_at_ = millis() + 30000;
  diagnostic_text_(8, "Boot; timestamps are UTC seconds, zero means clock unavailable");
  diagnostic_status_("Recording locally");
}
void HelioBridge::diagnostic_append_(uint8_t kind, const uint8_t *data, size_t size) {
  if (!diagnostic_handle_) return;
  if (size > 65535 || diagnostic_pending_.size() + size + 11 > MAX_PENDING) { ++diagnostic_dropped_; return; }
  const size_t offset = diagnostic_pending_.size();
  diagnostic_pending_.resize(offset + 11 + size);
  auto *p = diagnostic_pending_.data() + offset;
  p[0] = kind;
  protocol::write32(p + 1, clock_ && clock_->utcnow().is_valid() ? clock_->utcnow().timestamp : 0);
  protocol::write32(p + 5, millis()); p[9] = size; p[10] = size >> 8;
  if (size) memcpy(p + 11, data, size);
}
void HelioBridge::diagnostic_text_(uint8_t kind, const char *message) {
  diagnostic_append_(kind, reinterpret_cast<const uint8_t *>(message), strlen(message));
}
void HelioBridge::diagnostic_event(const char *message) { diagnostic_text_(8, message); }
bool HelioBridge::diagnostic_evict_() {
  size_t slot = DIAGNOSTIC_SLOTS; uint32_t oldest = UINT32_MAX;
  for (size_t i = 0; i < DIAGNOSTIC_SLOTS; i++)
    if (diagnostic_sequences_[i] && diagnostic_sequences_[i] < oldest) { oldest = diagnostic_sequences_[i]; slot = i; }
  if (slot == DIAGNOSTIC_SLOTS) return false;
  char key[12]; key_for(slot, key);
  if (nvs_erase_key(diagnostic_handle_, key) != ESP_OK || nvs_commit(diagnostic_handle_) != ESP_OK) return false;
  diagnostic_used_ -= diagnostic_sizes_[slot]; diagnostic_sizes_[slot] = 0; diagnostic_sequences_[slot] = 0;
  return true;
}
void HelioBridge::diagnostic_flush_() {
  if (!diagnostic_handle_ || diagnostic_pending_.empty()) return;
  auto compressed = diagnostic::encode(diagnostic_pending_.data(), diagnostic_pending_.size());
  std::vector<uint8_t> blob(16 + compressed.size());
  memcpy(blob.data(), "HLG2", 4);
  protocol::write32(blob.data() + 4, diagnostic_next_);
  protocol::write32(blob.data() + 8, diagnostic_pending_.size());
  protocol::write32(blob.data() + 12, diagnostic::crc(diagnostic_pending_.data(), diagnostic_pending_.size()));
  memcpy(blob.data() + 16, compressed.data(), compressed.size());
  nvs_stats_t stats{};
  const size_t need_entries = (blob.size() + 31) / 32 + 8;
  bool room = false;
  // Reserve at least 32 KiB for the alarm/settings namespaces and NVS GC.
  for (size_t i = 0; i <= DIAGNOSTIC_SLOTS; i++) {
    if (nvs_get_stats(nullptr, &stats) != ESP_OK) break;
    if (diagnostic_used_ + blob.size() + 96 <= STORAGE_BUDGET &&
        !diagnostic_sequences_[diagnostic_next_ % DIAGNOSTIC_SLOTS] && stats.free_entries > need_entries + 1024) { room = true; break; }
    if (!diagnostic_evict_()) break;
  }
  char key[12]; key_for(diagnostic_next_ % DIAGNOSTIC_SLOTS, key);
  if (!room || nvs_set_blob(diagnostic_handle_, key, blob.data(), blob.size()) != ESP_OK || nvs_commit(diagnostic_handle_) != ESP_OK) {
    ++diagnostic_dropped_; diagnostic_pending_.clear(); diagnostic_status_("Log write failed; alarm controller continues"); return;
  }
  const size_t slot = diagnostic_next_ % DIAGNOSTIC_SLOTS;
  diagnostic_sequences_[slot] = diagnostic_next_++;
  diagnostic_sizes_[slot] = blob.size() + 96; diagnostic_used_ += blob.size() + 96;
  diagnostic_pending_.clear(); diagnostic_status_("Recording locally");
}
void HelioBridge::download_diagnostics(int sequence) {
  if (!diagnostic_handle_) { ESP_LOGI("helio_log", "HLG2 ERROR storage_unavailable"); return; }
  if(sequence==-2){controller_snapshot_();return;}
  if(sequence==-3){controller_export_(diagnostic_pending_,"HLP2",diagnostic_next_);return;}
  if (sequence < 0) {
    // A download only reads committed batches; it never changes an alarm or forces a flush.
    uint32_t first = diagnostic_next_;
    for (auto seq : diagnostic_sequences_) if (seq && seq < first) first = seq;
    ESP_LOGI("helio_log", "HLG2 INDEX first=%u next=%u dropped=%u pending=%u", unsigned(first), unsigned(diagnostic_next_), unsigned(diagnostic_dropped_), unsigned(diagnostic_pending_.size()));
    return;
  }
  const size_t slot = uint32_t(sequence) % DIAGNOSTIC_SLOTS;
  if (!sequence || diagnostic_sequences_[slot] != uint32_t(sequence)) {
    ESP_LOGI("helio_log", "HLG2 MISSING seq=%u", unsigned(sequence)); return;
  }
  char key[12]; key_for(slot, key); size_t n = diagnostic_sizes_[slot] - 96;
  diagnostic_export_.resize(n);
  if (nvs_get_blob(diagnostic_handle_, key, diagnostic_export_.data(), &n) != ESP_OK) {
    diagnostic_export_.clear(); ESP_LOGI("helio_log", "HLG2 ERROR read_failed"); return;
  }
  diagnostic_export_sequence_ = sequence; diagnostic_export_offset_ = 0; diagnostic_export_at_ = millis() + 100;
  ESP_LOGI("helio_log", "HLG2 BEGIN seq=%u bytes=%u", unsigned(sequence), unsigned(n));
}
void HelioBridge::diagnostic_tick_() {
  const uint32_t now = millis();
  if (phase_ == Phase::IDLE && !queued_alarm_ && int32_t(now - diagnostic_flush_at_) >= 0) {
    diagnostic_flush_at_ = now + 30000; diagnostic_flush_();
  }
  if (diagnostic_export_.empty() || int32_t(now - diagnostic_export_at_) < 0) return;
  diagnostic_export_at_ = now + 100;
  size_t n = std::min(size_t(96), diagnostic_export_.size() - diagnostic_export_offset_);
  char hex[193]; static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) { auto b = diagnostic_export_[diagnostic_export_offset_ + i]; hex[i*2] = digits[b >> 4]; hex[i*2+1] = digits[b & 15]; }
  hex[2*n] = 0;
  ESP_LOGI("helio_log", "HLG2 DATA seq=%u offset=%u hex=%s", unsigned(diagnostic_export_sequence_), unsigned(diagnostic_export_offset_), hex);
  diagnostic_export_offset_ += n;
  if (diagnostic_export_offset_ == diagnostic_export_.size()) {
    ESP_LOGI("helio_log", "HLG2 END seq=%u", unsigned(diagnostic_export_sequence_)); diagnostic_export_.clear();
  }
}
}  // namespace esphome::helio_bridge
