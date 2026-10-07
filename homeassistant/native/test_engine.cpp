#define main engine_process_main
#include "engine.cpp"
#undef main
#include <cassert>
int main() {
  Engine e;const uint32_t now=1791351000;
  auto &b=e.bridge;b.smart_enabled_=true;b.clock_value.now=now;
  b.tick_smart(8.5,30);
  const uint32_t onset=now-8*3600;
  b.smart_snapshot_={};auto &d=b.smart_snapshot_;d.valid=d.accounting_complete=true;d.onset=onset;d.through=now;d.stage=4;
  d.through=now-300;b.smart_observe_(d,now-300);d.through=now;b.smart_observe_(d,now);
  b.model_prediction_.valid=true;b.model_prediction_.stage=5;b.model_prediction_.read_at=now;b.model_prediction_.sample_time=now-60;
  b.tick_smart(8.5,30);assert(b.smart_operation_);assert(b.smart_session_.attempted==smart_wake::minute_ceiling(onset+510*60));
  // Backfilled activity cannot displace a newer wear/stage observation.
  b.model_prediction_.read_at=now;b.wear_.read_at=now;
  const auto stage=b.model_prediction_.stage;const auto sample=b.model_prediction_.sample_time;
  e.activity({},now-300,now-300);assert(b.model_prediction_.stage==stage && b.model_prediction_.sample_time==sample && b.wear_.read_at==now);
  auto pending=e.checkpoint();Engine restarted;restarted.restore(pending);
  assert(restarted.bridge.smart_operation_ && restarted.bridge.phase_==HelioBridge::Phase::BUSY);
  restarted.bridge.ack(true);assert(restarted.bridge.owned_.valid && restarted.bridge.smart_session_.confirmed==pending.session.attempted);
  auto &r=restarted.bridge;r.smart_enabled_=true;r.clock_value.now=now+60;r.model_prediction_.valid=true;r.model_prediction_.stage=4;
  r.model_prediction_.read_at=now+60;r.model_prediction_.sample_time=now;r.smart_read_at_=now+60;r.smart_snapshot_.through=now+60;
  r.tick_smart(8.5,30);assert(r.smart_session_.early_selected && r.smart_session_.attempted==smart_wake::minute_ceiling(now+90));
  r.ack(true);const auto primary=r.follow_.confirmed;r.clock_value.now=primary+60;
  r.wear_={primary+60,primary+60,smart_wake::WearState::WORN,120,55};r.tick_smart(8.5,30);
  assert(r.follow_operation_ && r.follow_.attempted==primary+300);
  r.ack(true);r.clock_value.now=primary+120;r.wear_={primary+120,primary+120,smart_wake::WearState::REMOVED,115,0};r.tick_smart(8.5,30);
  assert(r.follow_.stopped && r.follow_.cancel_pending);r.tick_smart(8.5,30);assert(r.operation_==HelioBridge::Operation::CANCEL_ALARM);r.ack(true);
  assert(!r.follow_.confirmed);
  auto saved=restarted.checkpoint();Engine again;again.restore(saved);assert(again.bridge.follow_.stopped && !again.bridge.follow_.confirmed);
  again.bridge.smart_manual_override_();assert(again.bridge.smart_session_.manual_override);
  saved=again.checkpoint();Engine manual;manual.restore(saved);assert(manual.bridge.smart_session_.manual_override);
  // A corrupted score/learning checkpoint is rejected, not replaced with defaults.
  saved.score.format=999;bool rejected=false;try{manual.restore(saved);}catch(...){rejected=true;}assert(rejected);
  std::cout<<"Native HA policy: deep/light gate, 30-second rounding, restart, follow-up, removal, manual override and corrupt state passed\n";
}
