#include "stage_model.h"
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
using namespace esphome::helio_bridge;
int main(){uint32_t start,now;std::string hex;while(std::cin>>start>>now>>hex){
 std::vector<uint8_t> raw;for(size_t i=0;i<hex.size();i+=2)raw.push_back(std::stoul(hex.substr(i,2),nullptr,16));
 auto p=stage_model::from_records(raw.data(),raw.size(),start,now);
 std::cout<<p.valid<<' '<<unsigned(p.stage)<<' '<<p.sample_time;
 for(auto f:p.features)std::cout<<' '<<std::setprecision(17)<<f;
 for(auto f:p.scores)std::cout<<' '<<std::setprecision(17)<<f;
 std::cout<<'\n';}}
