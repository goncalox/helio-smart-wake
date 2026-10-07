// This process runs on the Home Assistant host, never on the ESP32.
// HA owns durable checkpoints, commands and audit logs; this is a pure model/policy worker.
#include "adapter.h"
#include "adaptive_model.h"
#include "sleep_score_model.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <stdexcept>
using namespace esphome::helio_bridge;

struct Learning {adaptive::State state{};std::array<adaptive::Night,adaptive::NIGHT_SLOTS> nights{};};
struct Checkpoint {
  uint32_t version{1};
  smart_wake::Session session{};smart_wake::FollowUp follow{};alarms::Owned owned{};
  alarms::Owned requested{};
  sleep_data::Snapshot snapshot{};stage_model::Prediction prediction{};smart_wake::Wear wear{};
  uint32_t read_at{},read_attempt{},candidate{},candidate_since{},retry_at{},now{},manual{};
  uint32_t pending{},pending_cancel{},pending_follow{},new_attempt{},enabled{},learn{1},automatic{1};
  Learning learning{};sleep_score::Model score{};
};
std::vector<uint8_t> unhex(const std::string &s) {
  if(s=="-")return {};
  if(s.size()%2 || s.size()>500000)throw std::runtime_error("Invalid hex size");
  std::vector<uint8_t> v(s.size()/2);
  for(size_t i=0;i<v.size();++i) {
    const auto a=s.substr(i*2,2);if(a.find_first_not_of("0123456789abcdefABCDEF")!=std::string::npos)throw std::runtime_error("Invalid hex");
    v[i]=std::stoul(a,nullptr,16);
  }return v;
}
template<class T> std::string hex(const T &v) {
  std::ostringstream out;out<<std::hex<<std::setfill('0');auto *p=reinterpret_cast<const uint8_t*>(&v);
  for(size_t i=0;i<sizeof(v);++i)out<<std::setw(2)<<unsigned(p[i]);return out.str();
}
bool valid_learning(const Learning &db) {if(!db.state.valid())return false;for(auto &n:db.nights)if(!n.valid())return false;return true;}
uint32_t night(uint32_t stamp) {
  auto t=esphome::ESPTime::from_epoch_local(stamp);bool previous=t.hour<18;
  t.hour=18;t.minute=t.second=0;t.recalc_timestamp_utc(false);
  t=esphome::ESPTime::from_epoch_utc(t.timestamp-(previous?86400:0));t.recalc_timestamp_local();return t.timestamp;
}
class Engine {
 public:
  HelioBridge bridge;adaptive::Learner learner;sleep_score::Model score;
  bool learning{true},automatic{true};uint32_t manual{},stamp{};
  Engine() {bridge.clock_value.now=1700000000;bridge.setup_smart_();}
  Checkpoint checkpoint() {
    Checkpoint p;p.session=bridge.smart_session_;p.follow=bridge.follow_;p.owned=bridge.owned_;
    p.requested=bridge.requested_;p.snapshot=bridge.smart_snapshot_;p.prediction=bridge.model_prediction_;p.wear=bridge.wear_;
    p.read_at=bridge.smart_read_at_;p.read_attempt=0;p.candidate=bridge.smart_candidate_;
    p.candidate_since=bridge.smart_candidate_since_;p.retry_at=bridge.smart_retry_at_;p.now=0;p.manual=manual;
    p.enabled=bridge.smart_enabled_;p.learn=learning;p.automatic=automatic;p.pending=bridge.smart_operation_;p.pending_cancel=bridge.operation_==HelioBridge::Operation::CANCEL_ALARM;
    p.pending_follow=bridge.follow_operation_;p.new_attempt=bridge.smart_new_attempt_;
    p.learning.state=learner.state;p.learning.nights=learner.nights;p.score=score;return p;
  }
  void restore(const Checkpoint &p) {
    if(p.version!=1 || !valid_learning(p.learning) || !p.score.valid() || p.session.version!=3 || p.follow.version!=1 || !p.session.settings.valid())throw std::runtime_error("Invalid checkpoint");
    bridge.smart_session_=p.session;bridge.follow_=p.follow;bridge.owned_=p.owned;
    bridge.requested_=p.requested;bridge.smart_snapshot_=p.snapshot;bridge.model_prediction_=p.prediction;bridge.wear_=p.wear;
    bridge.smart_read_at_=p.read_at;bridge.smart_read_attempt_at_=p.read_attempt;bridge.smart_candidate_=p.candidate;
    bridge.smart_candidate_since_=p.candidate_since;bridge.smart_retry_at_=p.retry_at;bridge.clock_value.now=p.now;manual=p.manual;
    bridge.smart_enabled_=p.enabled;learning=p.learn;automatic=p.automatic;bridge.smart_operation_=p.pending;bridge.operation_=p.pending_cancel?HelioBridge::Operation::CANCEL_ALARM:HelioBridge::Operation::SET_ALARM;
    bridge.follow_operation_=p.pending_follow;bridge.smart_new_attempt_=p.new_attempt;
    bridge.phase_=p.pending?HelioBridge::Phase::BUSY:HelioBridge::Phase::IDLE;
    learner.state=p.learning.state;learner.nights=p.learning.nights;score=p.score;
  }
  void import(const std::vector<uint8_t> &raw) {
    if(raw.size()<32 || protocol::read32(raw.data())!=1 || protocol::read32(raw.data()+4)!=sizeof(Learning) ||
       protocol::read32(raw.data()+8)!=sizeof(score) || protocol::read32(raw.data()+12)!=36 ||
       protocol::read32(raw.data()+16)!=24 || protocol::read32(raw.data()+20)!=sizeof(alarms::Owned) ||
       raw.size()!=32+sizeof(Learning)+sizeof(score)+36+24+sizeof(alarms::Owned))throw std::runtime_error("ESP snapshot ABI mismatch");
    Checkpoint p;size_t at=32;memcpy(&p.learning,raw.data()+at,sizeof(Learning));at+=sizeof(Learning);
    memcpy(&p.score,raw.data()+at,sizeof(score));at+=sizeof(score);memcpy(&p.session,raw.data()+at,36);at+=36;
    memcpy(&p.follow,raw.data()+at,24);at+=24;memcpy(&p.owned,raw.data()+at,sizeof(p.owned));
    p.now=protocol::read32(raw.data()+24);stamp=protocol::read32(raw.data()+28);restore(p);
  }
  void sleep(const std::vector<uint8_t> &raw,uint32_t now) {
    if(raw.size()%594 || raw.size()>594*32)throw std::runtime_error("Invalid sleep batch");
    sleep_data::Transfer transfer;transfer.start(raw.size(),now);uint8_t count=0;
    for(size_t at=0;at<raw.size();) {
      auto n=std::min(size_t(200),raw.size()-at);std::vector<uint8_t> frame(n+1);frame[0]=count++;std::copy_n(raw.begin()+at,n,frame.begin()+1);
      if(!transfer.feed(frame.data(),frame.size()))throw std::runtime_error("Sleep decode failed");at+=n;
    }
    if(!transfer.complete(false,0))throw std::runtime_error("Incomplete sleep batch");
    bridge.clock_value.now=std::max(bridge.clock_value.now,now);
    if(now>=bridge.smart_read_at_)bridge.smart_observe_(transfer.latest(),now);
    for(size_t at=0;at<raw.size();at+=594) {
      auto *p=raw.data()+at;auto midnight=protocol::read32(p+4);if(midnight<1577836800U)continue;
      uint32_t onset=midnight-86400+protocol::read16(p+10)*60U;if(onset>now || onset<1577836800U)continue;
      const auto key=night(onset);bool newest=true;
      for(auto &n:learner.nights)if(n.key==key && n.reference.observed>now)newest=false;
      if(learning && newest)learner.reference(key,p,594,now);
      newest=true;for(auto &n:score.nights)if(n.onset==onset && n.observed>now)newest=false;
      if(!newest)continue;
      auto local=esphome::ESPTime::from_epoch_local(onset);score.observe(p,594,now,local.hour*60+local.minute);
    }
  }
  void activity(const std::vector<uint8_t>&raw,uint32_t start,uint32_t now) {
    if(raw.size()%8 || raw.size()>120*8)throw std::runtime_error("Invalid activity batch");
    // Replay is archived in full, but older transfers must not replace live observations.
    if(now<bridge.model_prediction_.read_at || now<bridge.wear_.read_at)return;
    auto base=stage_model::from_records(raw.data(),raw.size(),start,now);
    if(learning && bridge.smart_enabled_ && !bridge.smart_session_.finished)learner.observe(night(base.sample_time),base);
    bridge.model_prediction_=learner.predict(base);
    bridge.wear_=smart_wake::wear_from_records(raw.data(),raw.size(),start,now);
    score.activity(raw.data(),raw.size(),start,now);
  }
  void tick(uint32_t now,float hours,int window,bool enabled,bool learn,bool autoupdate) {
    if(now<1577836800U)throw std::runtime_error("Clock invalid");
    bridge.clock_value.now=now;bridge.smart_enabled_=enabled;learning=learn;automatic=autoupdate;
    score.target_minutes=std::lround(hours*60);bridge.tick_smart(hours,window);
    bool live=bridge.smart_snapshot_.valid && !bridge.smart_snapshot_.is_nap && bridge.smart_snapshot_.through<=now && now-bridge.smart_snapshot_.through<=900;
    if(learning && !live && bridge.phase_==HelioBridge::Phase::IDLE) {
      learner.evaluate(now,automatic);learner.begin_training(now);while(learner.training)learner.train_slice(now,1024);
    }
  }
  void output(bool save=false) {
    const auto now=bridge.clock_value.now;auto s=bridge.smart_session_;auto f=bridge.follow_;
    const auto &p=bridge.model_prediction_;auto *q=score.latest();unsigned nights=0;for(auto &n:score.nights)if(n.onset)++nights;
    std::cout<<"{\"ok\":true,\"status\":"<<std::quoted(bridge.smart_message_)<<",\"now\":"<<now<<",\"onset\":"<<s.onset
      <<",\"attempted\":"<<s.attempted<<",\"follow_attempted\":"<<f.attempted<<",\"target\":"<<smart_wake::target(s)<<",\"confirmed\":"<<(f.primary?f.confirmed:s.confirmed)<<",\"awake\":"<<s.awake_minutes
      <<",\"pending\":"<<unsigned(bridge.smart_operation_)<<",\"cancel\":"<<(bridge.operation_==HelioBridge::Operation::CANCEL_ALARM)
      <<",\"follow\":"<<unsigned(bridge.follow_operation_)<<",\"epoch\":"<<(bridge.follow_operation_?f.attempted:s.attempted)
      <<",\"previous\":"<<bridge.smart_verified_epoch_()<<",\"fast\":"<<(smart_wake::light_window(s,now)||bridge.follow_monitoring_(now)||(s.confirmed>now&&s.confirmed-now<=900))
      <<",\"desired\":"<<smart_wake::desired_alarm(s,bridge.smart_snapshot_,now,bridge.smart_read_at_,p)
      <<",\"model_valid\":"<<p.valid<<",\"model_stage\":"<<unsigned(p.stage)<<",\"model_sample\":"<<p.sample_time
      <<",\"champion\":"<<learner.state.champion_version<<",\"candidate\":"<<learner.state.candidate_version<<",\"checked\":"<<learner.state.evaluated_nights
      <<",\"score\":"<<(q?q->ours:0)<<",\"score_coverage\":"<<(q?q->coverage:0)<<",\"score_nights\":"<<nights
      <<",\"manual\":"<<manual<<",\"import_stamp\":"<<stamp;
    if(save)std::cout<<",\"checkpoint\":"<<std::quoted(hex(checkpoint()));std::cout<<"}"<<std::endl;
  }
};
int main() {
  setenv("TZ","Europe/Lisbon",1);tzset();Engine engine;std::string line;
  while(std::getline(std::cin,line)) {
    try {
      std::istringstream in(line);std::string cmd,data;uint32_t now=0,start=0;in>>cmd;
      if(cmd=="IMPORT"){in>>data;engine.import(unhex(data));}
      else if(cmd=="RESTORE"){in>>data;auto v=unhex(data);if(v.size()!=sizeof(Checkpoint))throw std::runtime_error("Checkpoint size");Checkpoint p;memcpy(&p,v.data(),v.size());engine.restore(p);}
      else if(cmd=="SLEEP"){in>>now>>data;engine.sleep(unhex(data),now);}
      else if(cmd=="ACTIVITY"){in>>now>>start>>data;engine.activity(unhex(data),start,now);}
      else if(cmd=="TICK"){float hours=0;int window=0;bool enabled=false,learning=false,automatic=false;in>>now>>hours>>window>>enabled>>learning>>automatic;engine.tick(now,hours,window,enabled,learning,automatic);}
      else if(cmd=="ACK"){bool ok=false;in>>now>>ok;engine.bridge.clock_value.now=now;engine.bridge.ack(ok);}
      else if(cmd=="MANUAL"){in>>now;engine.bridge.clock_value.now=now;engine.bridge.smart_manual_override_();++engine.manual;engine.bridge.phase_=HelioBridge::Phase::IDLE;}
      else if(cmd!="SAVE" && cmd!="STATUS")throw std::runtime_error("Unknown command");
      if(in.fail() && cmd!="SAVE" && cmd!="STATUS")throw std::runtime_error("Malformed command");engine.output(cmd=="SAVE");
    } catch(const std::exception &e) {std::cout<<"{\"ok\":false,\"error\":"<<std::quoted(e.what())<<"}"<<std::endl;}
  }
  return 0;
}
