#include "helio_bridge.h"
#include "diagnostic_codec.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include <cstring>

namespace esphome::helio_bridge {
void HelioBridge::setup_remote_() {
  const auto address=parent()->get_address();
  remote_pref_=global_preferences->make_preference<remote::State>(0x48455231U ^ uint32_t(address) ^ uint32_t(address>>32));
  if(!remote_pref_.load(&remote_) || !remote::valid(remote_))remote_={};
  if(!remote_.owner){remote_.owner=1;save_remote_();}
  if(remote_.status==remote::PENDING) {remote_.status=remote::FAILED;save_remote_();}
}
bool HelioBridge::save_remote_() {return remote_pref_.save(&remote_) && global_preferences->sync();}
bool HelioBridge::remote_fast_() const {
  return remote_.owner && clock_ && clock_->utcnow().is_valid() && clock_->utcnow().timestamp<remote_fast_until_;
}
void HelioBridge::controller_fast(int expires) {
  if(!remote_.owner || !clock_ || !clock_->utcnow().is_valid())return;
  const uint32_t now=clock_->utcnow().timestamp;
  if(expires>=int(now) && uint64_t(expires)<=uint64_t(now)+120)remote_fast_until_=expires;
}
bool HelioBridge::controller_owner(bool home_assistant) {
  // Retained API compatibility for HA activation; ESP32 ownership is impossible.
  return home_assistant && remote_.owner == 1;
}
std::string HelioBridge::controller_status() const {
  char text[240];
  snprintf(text,sizeof(text),"{\"owner\":%u,\"id\":%u,\"epoch\":%u,\"status\":%u,\"previous\":%u,\"manual\":%u,\"idle\":%u,\"local_wake\":false}",
    unsigned(remote_.owner),unsigned(remote_.id),unsigned(remote_.epoch),unsigned(remote_.status),
    unsigned(remote_.previous),unsigned(remote_.manual),unsigned(phase_==Phase::IDLE && !queued_alarm_));
  return text;
}
void HelioBridge::controller_alarm(int id,int epoch,int expires,int previous,bool cancel,bool follow) {
  if(!remote_.owner || id<=0 || !clock_ || !clock_->utcnow().is_valid())return;
  const uint32_t now=clock_->utcnow().timestamp;
  if(uint32_t(id)<remote_.id || remote_inflight_ || phase_!=Phase::IDLE || queued_alarm_)return;
  if(uint32_t(id)==remote_.id && (remote_.epoch!=uint32_t(epoch) || remote_.cancel!=cancel || remote_.previous!=uint32_t(previous) || remote_.follow!=follow))return;
  if(uint32_t(id)==remote_.id && remote_.status==remote::VERIFIED)return;
  auto next=remote_;next.id=id;next.epoch=epoch;next.expires=expires;next.previous=previous;next.cancel=cancel;next.follow=follow;next.status=remote::PENDING;
  if(!remote::timely(next,now))return;
  const auto old=remote_;remote_=next;if(!save_remote_()){remote_=old;return;}
  remote_inflight_=true;smart_dispatch_=true;
  // Transport commands bypass local policy, while retaining owned-slot verification.
  if(cancel && !owned_.valid) {remote_result_(true);smart_dispatch_=false;return;}
  if(cancel)cancel_alarm();else {auto local=ESPTime::from_epoch_local(epoch);set_alarm(local.hour,local.minute,0);}
  smart_dispatch_=false;
}
void HelioBridge::remote_result_(bool ok) {
  if(!remote_inflight_)return;
  remote_inflight_=false;smart_operation_=false;
  remote_.status=ok?remote::VERIFIED:remote::FAILED;
  if(ok)remote_.previous=remote_.cancel?0:remote_.epoch;
  save_remote_();
  diagnostic_text_(8,ok?"Home Assistant command verified by alarm readback":"Home Assistant command not verified");
}
void HelioBridge::remote_manual_() {
  if(!remote_.owner)return;
  ++remote_.manual;remote_.status=remote::MANUAL;save_remote_();
}
void HelioBridge::controller_snapshot_() {
  struct Learning {adaptive::State state{};std::array<adaptive::Night,adaptive::NIGHT_SLOTS> nights{};};
  if(!learning_ || !quality_){ESP_LOGI("helio_log","HLG2 ERROR models_unavailable");return;}
  const size_t size=32+sizeof(Learning)+sizeof(*quality_)+sizeof(smart_session_)+sizeof(follow_)+sizeof(owned_);
  std::vector<uint8_t> raw(size);
  protocol::write32(raw.data(),1);protocol::write32(raw.data()+4,sizeof(Learning));protocol::write32(raw.data()+8,sizeof(*quality_));
  protocol::write32(raw.data()+12,sizeof(smart_session_));protocol::write32(raw.data()+16,sizeof(follow_));protocol::write32(raw.data()+20,sizeof(owned_));
  protocol::write32(raw.data()+24,clock_&&clock_->utcnow().is_valid()?clock_->utcnow().timestamp:0);protocol::write32(raw.data()+28,millis());
  auto *db=new(raw.data()+32) Learning();db->state=learning_->state;db->nights=learning_->nights;
  size_t at=32+sizeof(Learning);
  memcpy(raw.data()+at,quality_,sizeof(*quality_));at+=sizeof(*quality_);
  memcpy(raw.data()+at,&smart_session_,sizeof(smart_session_));at+=sizeof(smart_session_);
  memcpy(raw.data()+at,&follow_,sizeof(follow_));at+=sizeof(follow_);
  memcpy(raw.data()+at,&owned_,sizeof(owned_));
  controller_export_(raw,"HLS1",0);
}
void HelioBridge::controller_export_(const std::vector<uint8_t> &raw,const char *magic,uint32_t sequence) {
  auto compressed=diagnostic::encode(raw.data(),raw.size());
  diagnostic_export_.assign(16,0);memcpy(diagnostic_export_.data(),magic,4);
  protocol::write32(diagnostic_export_.data()+4,sequence);protocol::write32(diagnostic_export_.data()+8,raw.size());
  protocol::write32(diagnostic_export_.data()+12,diagnostic::crc(raw.data(),raw.size()));
  diagnostic_export_.insert(diagnostic_export_.end(),compressed.begin(),compressed.end());
  diagnostic_export_sequence_=sequence;diagnostic_export_offset_=0;diagnostic_export_at_=millis()+100;
  ESP_LOGI("helio_log","HLG2 BEGIN seq=%u bytes=%u",unsigned(sequence),unsigned(diagnostic_export_.size()));
}
}
