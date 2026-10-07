#include "remote_control.h"
#include <cassert>
#include <iostream>
using namespace esphome::helio_bridge;
int main() {
  remote::State s;s.owner=1;s.id=10;s.epoch=1800000060;s.expires=1800000100;
  assert(remote::valid(s));assert(remote::timely(s,1800000030));
  assert(!remote::timely(s,1800000059));assert(!remote::timely(s,1800000110));
  s.epoch+=86400;assert(!remote::timely(s,1800000030));
  s.epoch=1800000061;assert(!remote::timely(s,1800000030));
  s.cancel=1;assert(remote::timely(s,1800000030));s.owner=2;assert(!remote::valid(s));
  std::cout<<"Remote command expiry, future-minute bounds and state schema passed\n";
}
