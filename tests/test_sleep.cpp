#include "sleep_data.h"
#include <cassert>
#include <iostream>
using namespace esphome::helio_bridge;
using namespace esphome::helio_bridge::sleep_data;
using Record = std::array<uint8_t, RECORD_SIZE>;
constexpr uint32_t midnight = 1700006400U, base = midnight - 86400, now = base + 2040 * 60;
void u16(Record &r, unsigned at, unsigned value) { r[at] = value; r[at + 1] = value >> 8; }
void segment(Record &r, unsigned block, unsigned index, unsigned from, unsigned to, uint8_t stage) {
  const auto at = 0x56 + block * 250 + index * 5;
  u16(r, at, from); u16(r, at + 2, to); r[at + 4] = stage;
}
Record night() {
  Record r{};
  protocol::write32(r.data() + 4, midnight);
  u16(r, 10, 1320); u16(r, 12, 1800);
  u16(r, 0x24a, 60); u16(r, 0x24c, 240); u16(r, 0x24e, 180);
  r[0x54] = 3;
  segment(r, 0, 0, 1320, 1559, 4);
  segment(r, 0, 1, 1560, 1739, 5);
  segment(r, 0, 2, 1740, 1799, 8);
  return r;
}
int main() {
  auto r = night();
  Snapshot s;
  assert(decode(r.data(), r.size(), now, s));
  assert(s.onset == base + 1320 * 60 && s.end == base + 1800 * 60);
  assert(s.through == s.end && s.stage == 8 && s.sleep_minutes == 480);
  assert(s.accounting_complete && s.awake_minutes == 0 && s.timeline_sleep_minutes == 480);
  auto awake = r;
  awake[0x54] = 4;
  segment(awake, 0, 0, 1320, 1439, 4);
  segment(awake, 0, 1, 1440, 1469, 7);
  segment(awake, 0, 2, 1470, 1739, 5);
  segment(awake, 0, 3, 1740, 1799, 8);
  assert(decode(awake.data(), awake.size(), now, s));
  assert(s.accounting_complete && s.awake_minutes == 30 && s.timeline_sleep_minutes == 450);
  // Clip pre-onset wakefulness, rather than extending the alarm for time before sleep.
  auto clipped = r; clipped[0x54] = 2; u16(clipped, 10, 1330);
  segment(clipped, 0, 0, 1320, 1359, 7); segment(clipped, 0, 1, 1360, 1799, 4);
  assert(decode(clipped.data(), clipped.size(), now, s));
  assert(s.accounting_complete && s.awake_minutes == 30 && s.timeline_sleep_minutes == 440);
  // A one-minute hole is unknown, not sleep or awake time.
  auto gap = r; u16(gap, 0x56 + 5, 1561);
  assert(decode(gap.data(), gap.size(), now, s) && !s.accounting_complete);
  auto ongoing = r;
  u16(ongoing, 12, 65535);
  u16(ongoing, 0x24a, 0); u16(ongoing, 0x24c, 0); u16(ongoing, 0x24e, 0);
  assert(decode(ongoing.data(), ongoing.size(), now, s));
  assert(s.onset == base + 1320 * 60 && s.through == base + 1800 * 60 && s.sleep_minutes == 0);
  assert(s.accounting_complete && s.timeline_sleep_minutes == 480);
  // Do not count the unelapsed part of a currently open minute.
  auto open_minute = ongoing; open_minute[0x54] = 1;
  segment(open_minute, 0, 0, 1320, 1335, 7);
  assert(decode(open_minute.data(), open_minute.size(), base + 1335 * 60 + 30, s));
  assert(s.accounting_complete && s.awake_minutes == 15);
  auto nap = r;
  nap[0x17] = 1;
  u16(nap, 0x18, 2000); u16(nap, 0x1a, 2029); u16(nap, 0x1c, 30);
  nap[0x55] = 3;
  segment(nap, 1, 0, 1800, 1999, 0x80);
  segment(nap, 1, 1, 2000, 2009, 4);
  segment(nap, 1, 2, 2010, 2029, 5);
  assert(decode(nap.data(), nap.size(), now, s));
  assert(s.onset == base + 2000 * 60 && s.through == base + 2030 * 60 && s.stage == 5 && s.sleep_minutes == 30);
  assert(s.is_nap && !s.accounting_complete && s.awake_minutes == 0);
  auto bad = r;
  bad[0x54] = 51;
  assert(!decode(bad.data(), bad.size(), now, s));
  bad = r; bad[0x56 + 4] = 42;
  assert(!decode(bad.data(), bad.size(), now, s));
  bad = r; u16(bad, 0x56 + 2, 1200);
  assert(!decode(bad.data(), bad.size(), now, s));
  assert(!decode(r.data(), r.size() - 1, now, s));
  assert(!decode(r.data(), r.size(), base, s));
  assert(!decode(r.data(), r.size(), 0, s));
  Transfer transfer;
  assert(!transfer.start(595, now));
  assert(!transfer.start(RECORD_SIZE * (MAX_RECORDS + 1), now));
  assert(transfer.start(0, now) && transfer.complete(false, 0));
  std::vector<uint8_t> raw(r.begin(), r.end()); raw.insert(raw.end(), nap.begin(), nap.end());
  assert(transfer.start(raw.size(), now));
  for (unsigned i = 0; i < raw.size(); i++) {
    uint8_t packet[]{uint8_t(i), raw[i]};
    assert(transfer.feed(packet, sizeof(packet)));
    if (i == 200) assert(!transfer.complete(false, 0));
  }
  assert(transfer.complete(true, protocol::crc32(raw.data(), raw.size())));
  assert(!transfer.complete(true, 0));
  assert(transfer.records() == 2 && transfer.usable() == 2 && transfer.latest().stage == 5);
  assert(transfer.latest().onset == base + 2000 * 60);
  assert(transfer.start(RECORD_SIZE, now));
  uint8_t first[]{0, r[0]}, skipped[]{2, r[1]};
  assert(transfer.feed(first, sizeof(first)));
  assert(!transfer.feed(skipped, sizeof(skipped)) && !transfer.complete(false, 0));
  std::cout << "Sleep timestamps, night/nap stages, invalid records, stream bounds, counter wrap and CRC checks passed\n";
}
