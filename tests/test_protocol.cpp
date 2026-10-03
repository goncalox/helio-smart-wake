#include "protocol.h"
#include <cassert>
#include <iostream>
using namespace esphome::helio_bridge::protocol;
int main() {
  const char *digits = "123456789";
  assert(crc32(reinterpret_cast<const uint8_t *>(digits), 9) == 0xcbf43926U);
  auto one = frame(0x82, {4,2,0,2}, 4, 247, 1, false);
  assert(one.size() == 1);
  assert((one[0] == std::vector<uint8_t>{3,7,0,1,0,4,0,0,0,0x82,0,4,2,0,2}));
  for (uint16_t mtu : {23,64,247,517}) {
    std::vector<uint8_t> data(700);
    for (size_t i=0;i<data.size();i++) data[i]=uint8_t(i);
    auto packets = frame(0x29,data,data.size(),mtu,255,false);
    Receiver receiver;
    Message message;
    for (size_t i=0;i<packets.size();i++) {
      assert(packets[i].size()<=std::min<size_t>(512,mtu-3));
      int result=receiver.feed(packets[i].data(),packets[i].size(),message);
      assert(result==(i+1==packets.size()?1:0));
    }
    assert(message.data==data && message.endpoint==0x29 && message.handle==255 && message.ack);
  }
  Receiver receiver;
  Message result;
  auto parts=frame(0x82,std::vector<uint8_t>(67,0x42),67,23,1,false);
  assert(receiver.feed(parts[0].data(),parts[0].size(),result)==0);
  assert(receiver.feed(parts[2].data(),parts[2].size(),result)==-1);
  assert(receiver.feed(parts[1].data(),parts[1].size(),result)==-1);
  auto bad=one[0];
  bad[5]=255; bad[6]=255;
  assert(receiver.feed(bad.data(),bad.size(),result)==-1);
  bad=one[0]; bad.push_back(0);
  assert(receiver.feed(bad.data(),bad.size(),result)==-1);
  bad=one[0]; bad.pop_back();
  assert(receiver.feed(bad.data(),bad.size(),result)==-1);
  auto encrypted=frame(0x29,std::vector<uint8_t>(32,0),21,247,8,true);
  assert(receiver.feed(encrypted[0].data(),encrypted[0].size(),result)==1);
  assert(result.plain_size==21 && result.encrypted && result.data.size()==32);
  std::cout << "Protocol vectors, fragmentation, bounds and out-of-order handling passed\n";
}
