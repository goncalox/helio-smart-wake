#include "activity_data.h"
#include <cassert>
#include <iostream>
using namespace esphome::helio_bridge;
int main() {
 uint8_t date[]={0xea,7,10,2,12,0,0,4};
 assert(activity_data::timestamp(date)==1790938800); // 2026-10-02 11:00 UTC
 date[7]=uint8_t(-4); assert(activity_data::timestamp(date)==1790946000);
 date[3]=32; assert(!activity_data::timestamp(date)); date[3]=2;
 activity_data::Transfer t; const uint32_t now=1790946000;
 assert(!t.start(7,now,now)); assert(!t.start(968,now-8000,now));
 assert(!t.start(8,now+61,now)); assert(!t.start(24,now,now));
 assert(t.start(16,now-60,now));
 uint8_t a[]={0,1,2,3,60,0,0,0,0}, b[]={1,1,0,0,61,0,0,0,0};
 assert(t.feed(a,sizeof(a))); assert(!t.complete(false,0)); assert(t.feed(b,sizeof(b)));
 assert(t.complete(true,diagnostic::crc(t.raw.data(),t.raw.size()))); assert(!t.complete(true,123));
 assert(t.start(16,now-60,now)); assert(!t.feed(b,sizeof(b))); assert(!t.complete(false,0));
 assert(t.start(8,now,now)); assert(t.feed(a,sizeof(a))); assert(!t.feed(b,sizeof(b))); assert(!t.complete(false,0));
 assert(t.start(0,now,now)); assert(t.complete(false,0));
 std::cout<<"Activity timestamp/offset, bounds, packet order, length and CRC checks passed\n";
}
