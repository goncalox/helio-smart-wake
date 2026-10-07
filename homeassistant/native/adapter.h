#pragma once
#include "smart_wake.h"
#include "remote_control.h"
#include "alarms.h"
#include <ctime>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#define ESP_LOGI(...) ((void)0)
namespace esphome {
struct ESPTime {
  int year{}, month{}, day_of_month{}, hour{}, minute{}, second{};
  time_t timestamp{};
  static ESPTime make(time_t e, bool local) {
    tm t{}; if (local) localtime_r(&e, &t); else gmtime_r(&e, &t);
    return {t.tm_year+1900,t.tm_mon+1,t.tm_mday,t.tm_hour,t.tm_min,t.tm_sec,e};
  }
  static ESPTime from_epoch_local(time_t e) { return make(e,true); }
  static ESPTime from_epoch_utc(time_t e) { return make(e,false); }
  void recalc(bool local) {
    tm t{}; t.tm_year=year-1900; t.tm_mon=month-1; t.tm_mday=day_of_month;
    t.tm_hour=hour; t.tm_min=minute; t.tm_sec=second; t.tm_isdst=-1;
    timestamp=local?mktime(&t):timegm(&t);
  }
  void recalc_timestamp_utc(bool) { recalc(false); }
  void recalc_timestamp_local() { recalc(true); }
  bool is_valid() const { return year>=2020; }
};
namespace sensor { struct Sensor {float value{}; void publish_state(float v) {value=v;} }; }
namespace text_sensor { struct TextSensor { std::string value; void publish_state(const char *s) { value=s; } }; }
inline std::vector<uint8_t> persisted;
inline std::map<uint32_t,std::vector<uint8_t>> extra_persisted;
constexpr uint32_t SESSION_KEY=0x48454c53U ^ 123U;
struct ESPPreferenceObject {
  uint32_t key{SESSION_KEY};
  std::vector<uint8_t>& data() {return key==SESSION_KEY ? persisted : extra_persisted[key];}
  template<class T> bool load(T *v) { auto &d=data(); if(d.size()!=sizeof(T)) return false; memcpy(v,d.data(),sizeof(T)); return true; }
  template<class T> bool save(T *v) { auto &d=data(); d.assign((uint8_t*)v,(uint8_t*)v+sizeof(T)); return true; }
};
struct Prefs { template<class T> ESPPreferenceObject make_preference(uint32_t key) { return {key}; } bool sync() {return true;} };
inline Prefs prefs; inline Prefs *global_preferences=&prefs;
struct Clock { uint32_t now{}; ESPTime utcnow() {return ESPTime::from_epoch_utc(now);} };
namespace helio_bridge {
struct HelioBridge {
  remote::State remote_{};bool remote_inflight_{};
  void remote_manual_() {}
  void remote_result_(bool) {}

  enum class Phase { IDLE, BUSY, ALARMS, ALARM_VERIFY, ALARM_WRITE }; enum class Operation { SET_ALARM, CANCEL_ALARM, TEST };
  Phase phase_{Phase::IDLE}; Operation operation_{};
  struct Parent {uint64_t get_address() {return 123;}} parent_value;
  Parent* parent() {return &parent_value;}
  Clock clock_value; Clock *clock_{&clock_value};
  bool smart_enabled_{false}, smart_dispatch_{false}, smart_operation_{false}, queued_alarm_{false}, sleep_monitoring_{true};
  smart_wake::Session smart_session_{};
  stage_model::Prediction model_prediction_{};
  void log_early_gate_(uint32_t,uint32_t) {}
  sleep_data::Snapshot smart_snapshot_{};
  ESPPreferenceObject smart_pref_, follow_pref_;
  smart_wake::FollowUp follow_{}; smart_wake::Wear wear_{};
  bool follow_operation_{false}, smart_new_attempt_{false};
  bool save_follow_(); bool tick_follow_(uint32_t); bool follow_monitoring_(uint32_t) const;
  void stop_follow_(); void smart_unsent_(); uint32_t smart_verified_epoch_() const;
  text_sensor::TextSensor *smart_status_sensor_{nullptr}, *smart_alarm_sensor_{nullptr}, *smart_target_sensor_{nullptr};
  sensor::Sensor *smart_awake_sensor_{nullptr};
  uint32_t smart_read_at_{}, smart_read_attempt_at_{}, smart_candidate_{}, smart_candidate_since_{}, smart_retry_at_{};
  std::string smart_message_;
  smart_wake::Session diagnostic_last_session_{}; bool diagnostic_have_session_=false;
  void diagnostic_text_(uint8_t,const char*) {}
  void diagnostic_append_(uint8_t,const uint8_t*,size_t) {}
  alarms::Owned owned_{}, requested_{};
  ESPPreferenceObject alarm_pref_{0x42};
  bool alarm_available_{true}, alarm_encrypted_{true}, close_requested_{false};
  std::vector<uint8_t> command;
  std::string alarm_message;
  void alarm_status_(const char *m) { alarm_message=m; }
  void status_(const char*) {}
  void fail_(const char *m) {alarm_message=m;smart_result_(false);phase_=Phase::IDLE;}
  void send_(uint16_t,const std::vector<uint8_t>& data,bool) {command=data;}
  template<class F> void set_timeout(const char*,uint32_t,F) {}
  void request_alarms_(bool); void handle_alarms_(const std::vector<uint8_t>&);
  int writes{}, cancels{}, reads{};
  void publish_timestamp_(text_sensor::TextSensor*,uint32_t) {}
  void set_alarm(int h,int m,int repeat) {assert(smart_dispatch_); smart_operation_=true; operation_=Operation::SET_ALARM;phase_=Phase::BUSY; requested_={1,owned_.slot,uint8_t(h),uint8_t(m),uint8_t(repeat)};writes++;}
  void cancel_alarm() {assert(smart_dispatch_);smart_operation_=true;operation_=Operation::CANCEL_ALARM;phase_=Phase::BUSY;cancels++;}
  void read_sleep() {reads++;smart_read_attempt_at_=clock_->now;}
  void ack(bool ok=true) {if(ok&&operation_==Operation::SET_ALARM)owned_=requested_;smart_result_(ok);phase_=Phase::IDLE;if(ok&&operation_==Operation::CANCEL_ALARM)owned_={};}
  void setup_smart_(); bool save_smart_(); void smart_status_(const char*);void smart_publish_();void smart_manual_override_();void smart_result_(bool);bool smart_write_allowed_();
  void smart_observe_(const sleep_data::Snapshot&,uint32_t);void tick_smart(float,int);
  void tick() {tick_smart(8.5,15);}
};
}
}
