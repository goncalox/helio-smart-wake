#include "helio_bridge.h"
#include "esphome/core/log.h"
#include <cstring>

namespace esphome::helio_bridge {
void HelioBridge::update_model_(const uint8_t *raw, size_t size, uint32_t start) {
  const auto base=stage_model::from_records(raw,size,start,clock_->utcnow().timestamp);
  learning_observe_(base);
  model_prediction_=learning_&&learning_handle_ ? learning_->predict(base) : base;
  const uint32_t version=learning_&&learning_handle_?learning_->state.champion_version:0;
  const auto &p = model_prediction_;
  const char *message=p.valid ? sleep_data::stage_name(p.stage) : stage_model::reason_name(p.reason);
  if(model_stage_sensor_) model_stage_sensor_->publish_state(message);
  ESP_LOGI("helio_model", "Model %s %u: %s; sample=%u read=%u age=%d seconds",
      version?"adaptive":stage_model::VERSION,unsigned(version),message,unsigned(p.sample_time),unsigned(p.read_at),
      p.sample_time ? int(p.read_at)-int(p.sample_time) : -1);
  uint8_t record[48]{};
  record[0]=version?2:1;record[1]=p.valid;record[2]=p.stage;record[3]=p.reason;
  protocol::write32(record+4,p.sample_time);
  for(size_t i=0;i<9;++i) {
    const float value=i<5 ? p.features[i] : p.scores[i-5];
    memcpy(record+8+i*4,&value,4);
  }
  protocol::write32(record+44,version);
  diagnostic_append_(version?17:11,record,version?48:44);
}
}  // namespace esphome::helio_bridge
