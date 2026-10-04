#include "helio_bridge.h"
#include "esphome/core/log.h"
#include <cstring>

namespace esphome::helio_bridge {
void HelioBridge::update_model_(const uint8_t *raw, size_t size, uint32_t start) {
  model_prediction_ = stage_model::from_records(raw, size, start, clock_->utcnow().timestamp);
  const auto &p = model_prediction_;
  const char *message=p.valid ? sleep_data::stage_name(p.stage) : stage_model::reason_name(p.reason);
  if(model_stage_sensor_) model_stage_sensor_->publish_state(message);
  ESP_LOGI("helio_model", "Model %s: %s; sample=%u read=%u age=%d seconds",
      stage_model::VERSION,message,unsigned(p.sample_time),unsigned(p.read_at),
      p.sample_time ? int(p.read_at)-int(p.sample_time) : -1);
  uint8_t record[44]{};
  record[0]=1;record[1]=p.valid;record[2]=p.stage;record[3]=p.reason;
  protocol::write32(record+4,p.sample_time);
  for(size_t i=0;i<9;++i) {
    const float value=i<5 ? p.features[i] : p.scores[i-5];
    memcpy(record+8+i*4,&value,4);
  }
  diagnostic_append_(11,record,sizeof(record));
}
void HelioBridge::log_early_gate_(uint32_t now, uint32_t desired) {
  uint8_t flags=0;
  if(smart_wake::fresh(smart_snapshot_,now)) flags|=1;
  if(smart_read_at_<=now && now-smart_read_at_<=90) flags|=2;
  if(model_prediction_.valid) flags|=4;
  if(model_prediction_.wake_ready(now,smart_snapshot_.through)) flags|=8;
  const uint32_t target=smart_wake::target(smart_session_);
  if(desired && desired<target) flags|=16;
  if(gate_logged_through_==smart_snapshot_.through && gate_logged_model_read_==model_prediction_.read_at &&
      gate_logged_target_==target && gate_logged_desired_==desired && gate_logged_flags_==flags) return;
  gate_logged_through_=smart_snapshot_.through;gate_logged_model_read_=model_prediction_.read_at;
  gate_logged_target_=target;gate_logged_desired_=desired;gate_logged_flags_=flags;
  uint8_t record[24]={2,smart_snapshot_.stage,model_prediction_.stage,flags};
  protocol::write32(record+4,smart_snapshot_.through);
  protocol::write32(record+8,model_prediction_.sample_time);
  protocol::write32(record+12,model_prediction_.read_at);
  protocol::write32(record+16,target);protocol::write32(record+20,desired);
  diagnostic_append_(12,record,sizeof(record));
  ESP_LOGI("helio_model", "Early gate: strap=%s model=%s model_wake_ready=%u decision=%s",
      sleep_data::stage_name(smart_snapshot_.stage),model_prediction_.valid ? sleep_data::stage_name(model_prediction_.stage) : "Unavailable",
      !!(flags&8),flags&16 ? "save earlier alarm" : "retain full target");
}
}  // namespace esphome::helio_bridge
