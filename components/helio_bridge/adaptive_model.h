#pragma once
#include "stage_model.h"
#include "sleep_data.h"
#include <cstring>
#include <limits>
#include <type_traits>

namespace esphome::helio_bridge::adaptive {
constexpr size_t NIGHT_SLOTS=3, ROWS=192;
constexpr uint32_t LABEL_DELAY=12*3600, LABEL_STABILITY=3600;
constexpr uint32_t REQUIRED_NIGHTS=3, MIN_TEST_ROWS=80;
constexpr size_t TRAIN_EPOCHS=30;
using Weights=std::array<std::array<double,6>,4>;
using Matrix=std::array<std::array<uint32_t,4>,4>;
inline int index(uint8_t stage) {
  for(int i=0;i<4;++i) if(stage_model::STAGES[i]==stage) return i;
  return -1;
}
inline Weights initial_weights() {
  Weights w{};for(size_t i=0;i<4;++i)for(size_t j=0;j<6;++j)w[i][j]=stage_model::WEIGHTS[i][j];return w;
}
inline bool finite_weights(const Weights &w) {
  for(const auto &a:w)for(double v:a)if(!std::isfinite(v)||std::abs(v)>20) return false;
  return true;
}
inline bool transform(const std::array<double,5> &f,std::array<double,6> &z) {
  z[0]=1;
  for(size_t j=0;j<5;++j) {
    if(!std::isfinite(f[j])||f[j]<0) return false;
    double v=j>=3?std::log1p(f[j]):f[j];
    z[j+1]=std::clamp((v-stage_model::MEAN[j])/stage_model::SCALE[j],-8.,8.);
  }
  return f[0]>=30 && f[0]<=220 && f[1]>=30 && f[1]<=220;
}
inline stage_model::Prediction infer(const Weights &w,const std::array<double,5> &f) {
  stage_model::Prediction p;p.features=f;std::array<double,6> z{};
  if(!transform(f,z)) {p.reason=5;return p;}
  size_t best=0;
  for(size_t k=0;k<4;++k) {
    for(size_t j=0;j<6;++j)p.scores[k]+=w[k][j]*z[j];
    if(!std::isfinite(p.scores[k])){p.reason=5;return p;}
    if(p.scores[k]>p.scores[best]) best=k;
  }
  p.valid=true;p.reason=0;p.stage=stage_model::STAGES[best];return p;
}
struct Row {
  uint32_t sample{}, champion_version{}, candidate_version{};
  std::array<float,5> features{};
  uint8_t champion_stage{},candidate_stage{},label{},reserved{};
};
static_assert(sizeof(Row)==36,"Adaptive row layout changed");
struct Reference {
  uint32_t onset{},end{},observed{},changed{},signature{};
  uint8_t count{},reserved[3]{};
  // Night timeline only; no nap or provisional live label is used for training.
  std::array<uint8_t,250> segments{};
  uint32_t base{};
  bool ready(uint32_t now) const {
    return onset && end>onset && now>=uint64_t(end)+LABEL_DELAY &&
      observed>=uint64_t(end)+LABEL_DELAY && now>=uint64_t(changed)+LABEL_STABILITY;
  }
  uint8_t label(uint32_t sample) const {
    if(sample<onset || sample>=end) return 0;
    for(size_t i=0;i<count;++i) {
      const auto *p=segments.data()+i*5;
      const uint64_t lo=uint64_t(base)+protocol::read16(p)*60;
      const uint64_t hi=uint64_t(base)+(protocol::read16(p+2)+1U)*60;
      if(sample>=lo && sample<hi)return p[4];
    }
    return 0;
  }
};
struct Night {
  uint32_t key{};
  Reference reference{};
  uint16_t size{};uint8_t evaluated{},reserved{};
  std::array<Row,ROWS> rows{};
  bool valid() const {
    if(size>ROWS||reference.count>50||evaluated>1)return false;
    for(size_t i=0;i<size;++i) {
      const auto &r=rows[i];if(!r.sample||(i&&r.sample<=rows[i-1].sample))return false;
      for(float f:r.features)if(!std::isfinite(f)||f<0)return false;
    }
    return true;
  }
};
struct State {
  uint32_t format{1},champion_version{},candidate_version{},next_version{1},trained_through{},checked_through{};
  uint32_t evaluated_nights{},candidate_created{},promotions{},rejections{};
  Weights champion{initial_weights()},candidate{initial_weights()};
  Matrix champion_test{},candidate_test{};
  bool valid() const {
    uint64_t old_total=0,new_total=0;
    for(const auto &row:champion_test)for(auto value:row)old_total+=value;
    for(const auto &row:candidate_test)for(auto value:row)new_total+=value;
    return format==1 && next_version>champion_version && next_version>candidate_version &&
      (!candidate_version || candidate_version>champion_version) &&
      (candidate_version || !evaluated_nights) && evaluated_nights<=REQUIRED_NIGHTS &&
      old_total==new_total && old_total<=REQUIRED_NIGHTS*ROWS &&
      finite_weights(champion)&&finite_weights(candidate);
  }
};
struct Quality {
  uint32_t total{},wake_labels{},nonwake_labels{},wake_predictions{},correct_wake{},false_wake{};
  double agreement{},balanced{},wake_precision{},wake_recall{},false_wake_rate{};
};
inline Quality quality(const Matrix &m) {
  Quality q;uint32_t correct=0,classes=0;
  for(size_t a=0;a<4;++a) {
    uint32_t support=0;
    for(size_t b=0;b<4;++b) {
      const auto n=m[a][b];q.total+=n;support+=n;if(a==b)correct+=n;
      const bool awake_label=a==0||a==3,awake_prediction=b==0||b==3;
      if(awake_label)q.wake_labels+=n;else q.nonwake_labels+=n;
      if(awake_prediction) {q.wake_predictions+=n;if(awake_label)q.correct_wake+=n;else q.false_wake+=n;}
    }
    if(support){q.balanced+=double(m[a][a])/support;++classes;}
  }
  q.agreement=q.total?double(correct)/q.total:0;q.balanced=classes?q.balanced/classes:0;
  q.wake_precision=q.wake_predictions?double(q.correct_wake)/q.wake_predictions:0;
  q.wake_recall=q.wake_labels?double(q.correct_wake)/q.wake_labels:0;
  q.false_wake_rate=q.nonwake_labels?double(q.false_wake)/q.nonwake_labels:0;
  return q;
}
inline bool better(const Matrix &candidate,const Matrix &champion,uint32_t nights) {
  const auto a=quality(candidate),b=quality(champion);
  return nights>=REQUIRED_NIGHTS && a.total>=REQUIRED_NIGHTS*MIN_TEST_ROWS && a.total==b.total &&
    a.wake_labels>=40 && a.nonwake_labels>=40 && a.wake_predictions>=20 &&
    a.agreement>=b.agreement && a.balanced>=b.balanced+.03 &&
    a.wake_precision>=b.wake_precision+.03 && a.false_wake_rate<=b.false_wake_rate &&
    a.wake_recall>=b.wake_recall*.8;
}
class Learner {
 public:
  State state{};
  std::array<Night,NIGHT_SLOTS> nights{};
  std::array<bool,NIGHT_SLOTS> dirty{};
  bool state_dirty{},training{},eligible{};
  uint32_t event{},last_event{};
  enum {NONE,CANDIDATE_TRAINED,EVALUATED,ACCEPTED,REJECTED,INVALID,WAITING_APPROVAL};
  stage_model::Prediction predict(const stage_model::Prediction &base) const {
    if(!base.valid || !state.champion_version)return base;
    auto p=infer(state.champion,base.features);
    p.sample_time=base.sample_time;p.read_at=base.read_at;p.heart_rate_samples=base.heart_rate_samples;
    return p;
  }
  stage_model::Prediction shadow(const stage_model::Prediction &base) const {
    if(!base.valid || !state.candidate_version)return {};
    return infer(state.candidate,base.features);
  }
  size_t slot(uint32_t key) {
    for(size_t i=0;i<NIGHT_SLOTS;++i)if(nights[i].key==key)return i;
    size_t oldest=0;for(size_t i=0;i<NIGHT_SLOTS;++i)if(!nights[i].key || nights[i].key<nights[oldest].key)oldest=i;
    if(nights[oldest].key>key)return NIGHT_SLOTS;
    auto &replacement=nights[oldest];replacement.key=key;replacement.reference={};
    replacement.size=0;replacement.evaluated=0;replacement.reserved=0;replacement.rows.fill(Row{});
    dirty[oldest]=true;return oldest;
  }
  void observe(uint32_t key,const stage_model::Prediction &base) {
    if(!base.valid || !key || base.sample_time>base.read_at || base.read_at-base.sample_time>180)return;
    std::array<double,6> z{};if(!transform(base.features,z))return;
    const size_t i=slot(key);if(i==NIGHT_SLOTS)return;auto &n=nights[i];
    if(n.size&&n.rows[n.size-1].sample>=base.sample_time)return;
    if(n.size==ROWS){std::move(n.rows.begin()+1,n.rows.end(),n.rows.begin());--n.size;}
    auto &r=n.rows[n.size++];r={};r.sample=base.sample_time;r.champion_version=state.champion_version;
    r.candidate_version=state.candidate_version;
    for(size_t j=0;j<5;++j)r.features[j]=base.features[j];
    r.champion_stage=predict(base).stage;r.candidate_stage=shadow(base).stage;
    dirty[i]=true;
  }
  void reference(uint32_t key,const uint8_t *raw,size_t length,uint32_t now) {
    sleep_data::Snapshot decoded;
    if(!key||!sleep_data::decode(raw,length,now,decoded)||!raw[0x54])return;
    const uint32_t base=protocol::read32(raw+4)-86400;
    Reference r;r.base=base;r.count=raw[0x54];
    r.onset=base+protocol::read16(raw+10)*60U;
    // Use a finalized end marker; complete recorded night coverage is required.
    r.end=base+protocol::read16(raw+12)*60U;
    if(r.end<=r.onset || r.end-r.onset<2*3600 || r.end-r.onset>16*3600 || r.end>now)return;
    std::copy_n(raw+0x56,r.count*5,r.segments.begin());
    uint32_t cursor=r.onset;
    for(size_t j=0;j<r.count;++j) {
      const auto *p=r.segments.data()+j*5;
      const uint32_t lo=std::max(r.onset,base+protocol::read16(p)*60U);
      const uint32_t hi=std::min(r.end,base+(protocol::read16(p+2)+1U)*60U);
      if(hi<=lo)continue;if(lo!=cursor)return;cursor=hi;
    }
    if(cursor!=r.end)return;
    r.signature=protocol::crc32(r.segments.data(),r.segments.size()) ^ r.onset ^ r.end;
    const auto i=slot(key);if(i==NIGHT_SLOTS)return;
    auto &old=nights[i].reference;
    if(training && old.signature && old.signature!=r.signature) {training=false;event=INVALID;}
    r.changed=old.signature==r.signature?old.changed:now;r.observed=now;
    // Evaluation uses a settled reference once; later revisions do not rescore an already checked night.
    old=r;dirty[i]=true;
  }
  bool evaluate(uint32_t now,bool automatic) {
    event=NONE;
    if(training || !state.candidate_version)return false;
    if(state.evaluated_nights>=REQUIRED_NIGHTS) return conclude(automatic);
    std::array<size_t,NIGHT_SLOTS> order{};for(size_t i=0;i<NIGHT_SLOTS;++i)order[i]=i;
    std::sort(order.begin(),order.end(),[this](size_t a,size_t b){return nights[a].key<nights[b].key;});
    for(size_t i:order) {
      auto &n=nights[i];
      if(n.key<=std::max(state.trained_through,state.checked_through) || !n.reference.ready(now))continue;
      Matrix a{},b{};uint32_t count=0;
      for(size_t j=0;j<n.size;++j) {
        auto &r=n.rows[j];const auto label=n.reference.label(r.sample);const int actual=index(label);
        if(actual<0 || r.candidate_version!=state.candidate_version || r.champion_version!=state.champion_version)continue;
        const int candidate=index(r.candidate_stage),champion=index(r.champion_stage);
        if(candidate<0||champion<0)continue;
        a[actual][candidate]++;b[actual][champion]++;count++;
      }
      if(count<MIN_TEST_ROWS)continue;
      n.evaluated=1;dirty[i]=true;state.checked_through=std::max(state.checked_through,n.key);
      for(size_t x=0;x<4;++x)for(size_t y=0;y<4;++y){state.candidate_test[x][y]+=a[x][y];state.champion_test[x][y]+=b[x][y];}
      state.evaluated_nights++;event=EVALUATED;state_dirty=true;
      if(state.evaluated_nights>=REQUIRED_NIGHTS) return conclude(automatic);
    }
    return event!=NONE;
  }
  bool begin_training(uint32_t now) {
    if(training||state.candidate_version)return false;
    training_size_=0;counts_.fill(0);uint32_t newest=0;
    for(auto &n:nights) {
      if(!n.reference.ready(now))continue;
      for(size_t j=0;j<n.size;++j) {
        auto &r=n.rows[j];r.label=n.reference.label(r.sample);const int label=index(r.label);
        if(label<0)continue;training_rows_[training_size_++]=r;counts_[label]++;
      }
      newest=std::max(newest,n.key);
    }
    size_t classes=0;for(auto count:counts_)if(count>=10)classes++;
    if(training_size_<MIN_TEST_ROWS || classes<2 || newest<=state.trained_through)return false;
    trial_=state.champion;newest_=newest;cursor_=epoch_=0;random_=newest^state.next_version;shuffle_();training=true;event=NONE;return true;
  }
  // Bounded slices run only while Bluetooth is idle; training never changes active coefficients.
  bool train_slice(uint32_t now,size_t steps=32) {
    if(!training)return false;
    while(steps-- && training) {
      const auto &r=training_rows_[cursor_];const int label=index(r.label);
      std::array<double,5> f{};for(size_t j=0;j<5;++j)f[j]=r.features[j];
      std::array<double,6> z{};
      if(!transform(f,z)){training=false;event=INVALID;return true;}
      double scores[4]{},prob[4]{},highest=-1e30,total=0;
      for(size_t k=0;k<4;++k){for(size_t j=0;j<6;++j)scores[k]+=trial_[k][j]*z[j];highest=std::max(highest,scores[k]);}
      for(size_t k=0;k<4;++k){prob[k]=std::exp(scores[k]-highest);total+=prob[k];}
      const double balance=std::min(3.,double(training_size_)/(4*counts_[label]));
      for(size_t k=0;k<4;++k)for(size_t j=0;j<6;++j) {
        const double gradient=balance*(prob[k]/total-(int(k)==label))*z[j]+(j?.02*(trial_[k][j]-state.champion[k][j]):0);
        trial_[k][j]-=.005*std::clamp(gradient,-5.,5.);
      }
      if(++cursor_==training_size_) {cursor_=0;shuffle_();if(++epoch_==TRAIN_EPOCHS) {
        training=false;
        if(!finite_weights(trial_)){event=INVALID;return true;}
        state.candidate=trial_;state.candidate_version=state.next_version++;
        state.trained_through=newest_;state.candidate_created=now;state.checked_through=newest_;state.evaluated_nights=0;
        state.champion_test={};state.candidate_test={};state_dirty=true;eligible=false;event=CANDIDATE_TRAINED;
      }}
    }
    return !training;
  }
 private:
  bool conclude(bool automatic) {
    eligible=better(state.candidate_test,state.champion_test,state.evaluated_nights);
    if(eligible && !automatic){event=WAITING_APPROVAL;return true;}
    if(eligible){state.champion=state.candidate;state.champion_version=state.candidate_version;state.promotions++;event=ACCEPTED;}
    else{state.rejections++;event=REJECTED;}
    state.candidate_version=0;state.evaluated_nights=0;state_dirty=true;
    for(auto &night:nights)night.evaluated=0;
    dirty.fill(true);return true;
  }
  void shuffle_() {
    for(size_t i=training_size_;i>1;--i) {
      random_^=random_<<13;random_^=random_>>17;random_^=random_<<5;
      std::swap(training_rows_[i-1],training_rows_[random_%i]);
    }
  }
  uint32_t random_{1};
  Weights trial_{};
  std::array<Row,NIGHT_SLOTS*ROWS> training_rows_{};
  std::array<uint32_t,4> counts_{};
  size_t training_size_{},cursor_{},epoch_{};
  uint32_t newest_{};
};
static_assert(std::is_trivially_copyable_v<Night> && std::is_trivially_copyable_v<State>,"Adaptive storage must remain plain data");
}  // namespace esphome::helio_bridge::adaptive
