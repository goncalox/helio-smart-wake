#include "helio_bridge.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include <esp_heap_caps.h>
#include <new>
#include <cstring>

namespace esphome::helio_bridge {
namespace {
struct Database {adaptive::State state{};std::array<adaptive::Night,adaptive::NIGHT_SLOTS> nights{};};
constexpr uint32_t MAGIC=0x314c4548U;
}
uint32_t HelioBridge::learning_night_(uint32_t epoch) const {
  if(!epoch)return 0;
  auto t=ESPTime::from_epoch_local(epoch);
  const bool previous=t.hour<18;t.hour=18;t.minute=0;t.second=0;
  t.recalc_timestamp_utc(false);
  t=ESPTime::from_epoch_utc(t.timestamp-(previous?86400:0));t.recalc_timestamp_local();return t.timestamp;
}
void HelioBridge::learning_status_(const char *message) {
  if(learning_message_==message)return;learning_message_=message;
  diagnostic_text_(15,message);ESP_LOGI("helio_learn","%s",message);
  if(learning_status_sensor_)learning_status_sensor_->publish_state(message);
}
void HelioBridge::setup_learning_() {
  void *memory=heap_caps_malloc(sizeof(adaptive::Learner),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  if(!memory && heap_caps_get_free_size(MALLOC_CAP_8BIT)>sizeof(adaptive::Learner)+65536)
    memory=heap_caps_malloc(sizeof(adaptive::Learner),MALLOC_CAP_8BIT);
  if(!memory){learning_status_("Learning unavailable: insufficient memory; fixed model retained");return;}
  learning_=new(memory) adaptive::Learner();
  if(nvs_open("helio_learn_v1",NVS_READWRITE,&learning_handle_)!=ESP_OK) {
    learning_handle_=0;learning_status_("Learning storage unavailable; fixed model retained");return;
  }
  size_t size=0;
  if(nvs_get_blob(learning_handle_,"database",nullptr,&size)==ESP_OK) {
    bool good=size==sizeof(Database)+16;
    std::vector<uint8_t> blob(good?size:0);
    if(good)new(blob.data()+16) Database();
    if(good)good=nvs_get_blob(learning_handle_,"database",blob.data(),&size)==ESP_OK &&
      protocol::read32(blob.data())==MAGIC && protocol::read32(blob.data()+4)==1 &&
      protocol::read32(blob.data()+8)==sizeof(Database) &&
      protocol::crc32(blob.data()+16,sizeof(Database))==protocol::read32(blob.data()+12);
    if(good) {
      auto *db=reinterpret_cast<const Database*>(blob.data()+16);
      good=db->state.valid();for(const auto &night:db->nights)good=good&&night.valid();
      if(good){learning_->state=db->state;learning_->nights=db->nights;}
    }
    if(!good)learning_status_("Learning state rejected; fixed model retained, collecting new nights");
  }
  learning_flush_at_=millis()+3600000;learning_tick_at_=millis()+60000;
  learning_publish_();
}
bool HelioBridge::save_learning_() {
  if(!learning_||!learning_handle_)return false;
  std::vector<uint8_t> blob(sizeof(Database)+16);
  protocol::write32(blob.data(),MAGIC);protocol::write32(blob.data()+4,1);protocol::write32(blob.data()+8,sizeof(Database));
  // Value-initialize the complete image, including reserved bytes; one atomic NVS blob.
  auto *db=new(blob.data()+16) Database();db->state=learning_->state;db->nights=learning_->nights;
  protocol::write32(blob.data()+12,protocol::crc32(blob.data()+16,sizeof(Database)));
  nvs_stats_t stats{};
  const size_t need=(blob.size()+31)/32+16;
  for(size_t i=0;i<=DIAGNOSTIC_SLOTS;++i) {
    if(nvs_get_stats(nullptr,&stats)!=ESP_OK)return false;
    if(stats.free_entries>need+1024)break;
    if(!diagnostic_evict_())return false;
  }
  if(stats.free_entries<=need+1024 || nvs_set_blob(learning_handle_,"database",blob.data(),blob.size())!=ESP_OK || nvs_commit(learning_handle_)!=ESP_OK) {
    learning_status_("Learning save failed; active model unchanged");return false;
  }
  if(learning_->state_dirty) {
    const auto &s=learning_->state;
    std::vector<uint8_t> audit(548);
    const uint32_t values[]={1,s.champion_version,s.candidate_version,s.trained_through,s.checked_through,s.evaluated_nights,s.promotions,s.rejections,s.candidate_created};
    for(size_t i=0;i<9;++i)protocol::write32(audit.data()+i*4,values[i]);
    size_t at=36;
    for(const auto *w:{&s.champion,&s.candidate})for(const auto &row:*w)for(double value:row){memcpy(audit.data()+at,&value,8);at+=8;}
    for(const auto *m:{&s.champion_test,&s.candidate_test})for(const auto &row:*m)for(uint32_t value:row){protocol::write32(audit.data()+at,value);at+=4;}
    diagnostic_append_(18,audit.data(),audit.size());
  }
  learning_->state_dirty=false;learning_->dirty.fill(false);learning_flush_at_=millis()+3600000;return true;
}
void HelioBridge::learning_publish_() {
  if(!learning_)return;
  if(!learning_handle_){learning_status_("Learning storage unavailable; fixed model retained");return;}
  char message[180],active[40];const auto &s=learning_->state;
  if(s.champion_version)snprintf(active,sizeof(active),"Learned model %u",unsigned(s.champion_version));
  else snprintf(active,sizeof(active),"Original model");
  if(!learning_enabled_)snprintf(message,sizeof(message),"Learning off; %s retained",active);
  else if(learning_->training)snprintf(message,sizeof(message),"Training an update; %s retained",active);
  else if(s.candidate_version)snprintf(message,sizeof(message),"%s; update %u: %u/3 later nights checked%s",active,unsigned(s.candidate_version),unsigned(s.evaluated_nights),learning_->eligible&&!learning_automatic_?"; passed, automatic updates off":"");
  else snprintf(message,sizeof(message),"%s; collecting settled nights; %u updates accepted, %u rejected",active,unsigned(s.promotions),unsigned(s.rejections));
  learning_status_(message);
}
void HelioBridge::learning_observe_(const stage_model::Prediction &base) {
  if(!learning_||!learning_handle_)return;
  if(learning_enabled_ && !remote_.owner)learning_->observe(learning_night_(base.sample_time),base);
  const auto candidate=learning_->shadow(base),champion=learning_->predict(base);
  uint8_t record[20]={1,champion.stage,candidate.stage,uint8_t(base.valid)};
  protocol::write32(record+4,base.sample_time);protocol::write32(record+8,learning_->state.champion_version);
  protocol::write32(record+12,learning_->state.candidate_version);protocol::write32(record+16,base.read_at);
  diagnostic_append_(16,record,sizeof(record));
}
void HelioBridge::learning_reference_(const uint8_t *raw,size_t size,uint32_t now) {
  if(remote_.owner)return;
  if(!learning_||!learning_enabled_||!learning_handle_)return;
  for(size_t at=0;at+sleep_data::RECORD_SIZE<=size;at+=sleep_data::RECORD_SIZE) {
    const auto *p=raw+at;const uint32_t midnight=protocol::read32(p+4);
    if(midnight<1577836800U)continue;
    const uint64_t onset=uint64_t(midnight)-86400+protocol::read16(p+10)*60U;
    if(onset>now || onset<1577836800U)continue;
    learning_->reference(learning_night_(onset),p,sleep_data::RECORD_SIZE,now);
  }
}
void HelioBridge::learning_tick_() {
  if(!learning_||!learning_handle_||!clock_||!clock_->utcnow().is_valid()||phase_!=Phase::IDLE||queued_alarm_)return;
  const uint32_t ms=millis(),now=clock_->utcnow().timestamp;
  const bool live_sleep=smart_snapshot_.valid && !smart_snapshot_.is_nap && smart_snapshot_.through<=now && now-smart_snapshot_.through<=15*60;
  if(learning_enabled_ && !live_sleep) {
    if(learning_->training) {
      const auto previous=learning_->state;
      if(learning_->train_slice(now)) {
        if(learning_->state_dirty && !save_learning_()){learning_->state=previous;return;}
        learning_publish_();
      }
    } else if(int32_t(ms-learning_tick_at_)>=0) {
      learning_tick_at_=ms+60000;
      const auto previous=learning_->state;
      learning_->evaluate(now,learning_automatic_);
      if(learning_->state_dirty && !save_learning_()) {learning_->state=previous;return;}
      learning_->begin_training(now);learning_publish_();
    }
  }
  bool dirty=learning_->state_dirty;for(bool value:learning_->dirty)dirty=dirty||value;
  if(dirty && int32_t(ms-learning_flush_at_)>=0)save_learning_();
}
} // namespace esphome::helio_bridge
