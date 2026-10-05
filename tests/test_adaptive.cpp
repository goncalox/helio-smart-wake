#include "adaptive_model.h"
#include <cassert>
#include <iostream>
using namespace esphome::helio_bridge;
constexpr uint32_t BASE=1791162000;
std::array<uint8_t,594> reference(uint32_t onset,uint8_t last=8) {
 std::array<uint8_t,594> p{};
 auto w16=[&](size_t at,uint16_t v){p[at]=v;p[at+1]=v>>8;};
 protocol::write32(p.data()+4,onset+86400);
 w16(10,0);w16(12,360);w16(0x24a,240);w16(0x24c,120);
 p[0x54]=3;
 for(int i=0;i<3;++i){w16(0x56+i*5,i*120);w16(0x58+i*5,i*120+119);p[0x5a+i*5]=i==0?4:i==1?5:last;}
 return p;
}
stage_model::Prediction observation(uint32_t t,int value=0) {
 stage_model::Prediction p;p.valid=true;p.stage=4;p.sample_time=t;p.read_at=t+70;
 p.features={double(50+value),double(50+value),1.,1.,0.};return p;
}
void settled(adaptive::Learner &l,uint32_t key,uint32_t onset) {
 auto raw=reference(onset);const uint32_t at=onset+6*3600+adaptive::LABEL_DELAY;
 l.reference(key,raw.data(),raw.size(),at);l.reference(key,raw.data(),raw.size(),at+adaptive::LABEL_STABILITY);
}
int main() {
 adaptive::Learner l;auto b=observation(BASE);assert(l.predict(b).stage==b.stage);
 assert(l.state.valid() && sizeof(adaptive::State)+3*sizeof(adaptive::Night)<24576);
 for(int i=0;i<100;++i)l.observe(1,observation(BASE+i*180,i/40));
 const auto first=l.slot(1);assert(l.nights[first].size==100);l.observe(1,observation(BASE+99*180));assert(l.nights[first].size==100);
 auto raw=reference(BASE);const auto ended=BASE+6*3600;
 l.reference(1,raw.data(),raw.size(),ended+3599);assert(!l.begin_training(ended+adaptive::LABEL_DELAY+adaptive::LABEL_STABILITY)); // Old observed-at timestamp cannot supply future labels.
 raw=reference(BASE,7);l.reference(1,raw.data(),raw.size(),ended+adaptive::LABEL_DELAY);
 assert(!l.begin_training(ended+adaptive::LABEL_DELAY+adaptive::LABEL_STABILITY-1));
 l.reference(1,raw.data(),raw.size(),ended+adaptive::LABEL_DELAY+adaptive::LABEL_STABILITY);
 assert(l.begin_training(ended+adaptive::LABEL_DELAY+adaptive::LABEL_STABILITY));
 const auto original=l.state.champion;const uint32_t now=ended+adaptive::LABEL_DELAY+adaptive::LABEL_STABILITY;
 int slices=0;while(l.training){l.train_slice(now,32);assert(l.state.champion==original);++slices;}
 assert(slices>1 && l.state.candidate_version==1 && !l.state.champion_version && l.state.trained_through==1);
 assert(!l.begin_training(now));assert(!l.evaluate(now,true)); // Training night is never its own test.
 // Predictions are captured before later labels arrive; each test is a different dated night.
 for(uint32_t key=2;key<=4;++key) {
  const auto onset=BASE+(key-1)*86400;
  for(int i=0;i<100;++i)l.observe(key,observation(onset+i*180,i/40));
  settled(l,key,onset);const auto at=onset+6*3600+adaptive::LABEL_DELAY+adaptive::LABEL_STABILITY;
  assert(l.evaluate(at,true));
  if(key<4){assert(l.state.evaluated_nights==key-1);assert(!l.evaluate(at,true));}
 }
 assert(l.state.promotions+l.state.rejections==1 && !l.state.candidate_version);
 // Conservative activation: accuracy/balance/precision improve, false wakes do not increase.
 adaptive::Matrix champion{},candidate{};
 champion[0][0]=90;champion[0][1]=30;champion[1][0]=30;champion[1][1]=30;champion[2][0]=30;champion[2][2]=30;
 candidate[0][0]=110;candidate[0][1]=10;candidate[1][0]=10;candidate[1][1]=50;candidate[2][0]=10;candidate[2][2]=50;
 assert(!adaptive::better(candidate,champion,2));assert(adaptive::better(candidate,champion,3));
 auto bad=candidate;bad[1][0]=55;bad[1][1]=5;assert(!adaptive::better(bad,champion,3));
 assert(!adaptive::better(champion,champion,3));
 // Manual mode freezes a qualified candidate and cannot activate until enabled.
 adaptive::Learner approval;approval.state.candidate_version=1;approval.state.next_version=2;approval.state.evaluated_nights=3;
 approval.state.candidate_test=candidate;approval.state.champion_test=champion;
 assert(approval.evaluate(now,false) && approval.eligible && approval.state.champion_version==0);
 assert(approval.evaluate(now,true) && approval.state.champion_version==1);
 // Changed references reset the settling period; malformed/future/worn inputs cannot train.
 adaptive::Learner revisions;
 for(int i=0;i<100;++i)revisions.observe(7,observation(BASE+i*180));settled(revisions,7,BASE);
 auto changed=reference(BASE,4);revisions.reference(7,changed.data(),changed.size(),now+10);
 assert(!revisions.begin_training(now+11));
 auto invalid=observation(BASE);invalid.valid=false;revisions.observe(8,invalid);
 invalid=observation(BASE);invalid.features[0]=std::numeric_limits<double>::quiet_NaN();revisions.observe(8,invalid);
 assert(revisions.slot(8)<adaptive::NIGHT_SLOTS);for(auto &n:revisions.nights)if(n.key==8)assert(!n.size);
 auto broken=reference(BASE);broken[0x54]=51;revisions.reference(9,broken.data(),broken.size(),now);assert(!revisions.begin_training(now+20));
 // A late reference revision interrupts a training slice and cannot change active weights.
 adaptive::Learner interrupted;
 for(int i=0;i<100;++i)interrupted.observe(1,observation(BASE+i*180,i/40));settled(interrupted,1,BASE);
 assert(interrupted.begin_training(now));interrupted.train_slice(now,1);
 auto revised=reference(BASE,7);interrupted.reference(1,revised.data(),revised.size(),now+5);
 assert(!interrupted.training && !interrupted.state.candidate_version && !interrupted.state.champion_version);
 auto corrupt=l.state;corrupt.champion[0][0]=NAN;assert(!corrupt.valid());corrupt=l.state;corrupt.next_version=0;assert(!corrupt.valid());
 // Storage is bounded even with longer sessions; chronological newest rows survive.
 adaptive::Learner bounded;for(int i=0;i<300;++i)bounded.observe(1,observation(BASE+i*60));
 assert(bounded.nights[bounded.slot(1)].size==adaptive::ROWS && bounded.nights[bounded.slot(1)].rows[0].sample==BASE+108*60);
 std::cout<<"Adaptive learning: settled labels, sliced training, causal forward nights, conservative activation, approval mode, duplicate exclusion, invalid input and bounded storage passed\n";
}
