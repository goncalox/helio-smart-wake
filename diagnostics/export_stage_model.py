import argparse
import json
from pathlib import Path
parser=argparse.ArgumentParser(description='Export fixed model coefficients to a C++ header')
parser.add_argument('--model', type=Path, default=Path('diagnostics/deployed-model.json'))
parser.add_argument('--output', type=Path, required=True)
args=parser.parse_args()
m=json.loads(args.model.read_text())
assert m['feature_names']==['heart_rate','hr_mean_5m','hr_sd_5m','intensity_mean_5m','steps_5m']
assert m['classes']==['Light','Deep','REM','Awake']
array=lambda a:'{'+', '.join(format(v,'.17g') for v in a)+'}'
text='''#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace esphome::helio_bridge::stage_model {
// Experimental full-night fit from 2026-10-03; scores are not calibrated probabilities.
inline constexpr const char *VERSION = "20261003-v1";
'''
text+='inline constexpr std::array<double,5> MEAN = '+array(m['mean'])+';\n'
text+='inline constexpr std::array<double,5> SCALE = '+array(m['scale'])+';\n'
text+='inline constexpr double WEIGHTS[4][6] = {'+',\n'.join(array(w) for w in m['weights'])+'};\n'
text+='''inline constexpr uint8_t STAGES[4] = {4,5,8,7};
struct Prediction {
  bool valid{false}; uint8_t stage{0}, reason{1}, heart_rate_samples{0};
  uint32_t sample_time{0}, read_at{0};
  std::array<double,5> features{};
  std::array<double,4> scores{};
  bool light_ready(uint32_t now, uint32_t band_through) const {
    const int64_t alignment = int64_t(band_through) - (int64_t(sample_time) + 60);
    return valid && stage == 4 && sample_time <= now && now-sample_time <= 180 &&
        read_at <= now && now-read_at <= 90 && band_through && alignment >= -60 && alignment <= 60;
  }
};
inline const char *reason_name(uint8_t reason) {
  switch(reason) {
    case 0: return "Ready";
    case 1: return "Insufficient activity records";
    case 2: return "Missing heart rate";
    case 3: return "Strap not worn";
    case 4: return "Stale or future activity";
    default: return "Invalid model input";
  }
}
inline Prediction infer(const std::array<double,5> &features) {
  Prediction p; p.features = features;
  for (double v:features) if (!std::isfinite(v)) {p.reason=5; return p;}
  double z[6] = {1};
  for(size_t j=0;j<5;++j) {
    if (j>=3 && features[j]<0) {p.reason=5;return p;}
    const double value=j>=3 ? std::log1p(features[j]) : features[j];
    z[j+1]=(value-MEAN[j])/SCALE[j];
  }
  size_t best=0;
  for(size_t k=0;k<4;++k) {
    for(size_t j=0;j<6;++j) p.scores[k]+=WEIGHTS[k][j]*z[j];
    if(p.scores[k]>p.scores[best]) best=k;
  }
  p.valid=true;p.stage=STAGES[best];p.reason=0;return p;
}
inline Prediction from_records(const uint8_t *raw, size_t size, uint32_t start, uint32_t now) {
  Prediction p;p.read_at=now;
  if(size<40 || size%8) return p;
  p.sample_time=start+(size/8-1)*60;
  if(!start || p.sample_time>now || now-p.sample_time>180) {p.reason=4;return p;}
  const uint8_t *latest=raw+size-8;
  if(latest[0]==115 || latest[0]==118 || latest[0]==255) {p.reason=3;return p;}
  if(!latest[3] || latest[3]==255) {p.reason=2;return p;}
  double total=0, sum_squares=0, intensity=0, steps=0;
  uint8_t n=0;
  for(size_t at=size-40;at<size;at+=8) {
    intensity+=raw[at+1];steps+=raw[at+2];
    if(raw[at+3] && raw[at+3]!=255) {total+=raw[at+3];sum_squares+=double(raw[at+3])*raw[at+3];++n;}
  }
  if(n<3) {p.reason=2;return p;}
  const double mean=total/n;
  p=infer({double(latest[3]),mean,std::sqrt(std::max(0.,sum_squares/n-mean*mean)),intensity/5,steps});
  p.sample_time=start+(size/8-1)*60;p.read_at=now;p.heart_rate_samples=n;return p;
}
}  // namespace esphome::helio_bridge::stage_model
'''
# std::max declaration is intentionally explicit for host and ESP builds.
text=text.replace('#include <array>','#include <array>\n#include <algorithm>')
args.output.write_text(text)
print('Exported the full-night model coefficients for ESP32 inference.')
