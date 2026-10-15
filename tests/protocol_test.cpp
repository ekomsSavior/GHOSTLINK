#include "ghost_protocol.h"
#include <cassert>
#include <cstdio>
#include <random>
#include <set>
#include <utility>
using namespace ghost;

static void clocks() {
  SlotClock c;
  c.slot = 7; c.start = 1000;
  c.advance(1000 + 3 * HOP_INTERVAL_MS + 137);
  assert(c.slot == 10 && c.phase(6537) == 137);
  c.sync(90, 1750, 5000, 5250);
  assert(c.slot == 91 && c.phase(5250) == 200);
  c.start = 0xfffffff0u; c.slot = 123;
  c.advance(uint32_t(0xfffffff0u + HOP_INTERVAL_MS + 19));
  assert(c.slot == 124 && c.phase(uint32_t(0xfffffff0u + HOP_INTERVAL_MS + 19)) == 19);
  for (uint16_t phase = 0; phase < HOP_INTERVAL_MS; ++phase) {
    assert(!(txWindow(0, phase, false) && txWindow(1, phase, false)));
    if (phase < HOP_GUARD_MS || phase >= HOP_INTERVAL_MS - HOP_GUARD_MS)
      assert(!txWindow(0, phase, true) && !txWindow(1, phase, true));
  }
}

static GhostPacket validMessage() {
  GhostPacket p{};
  memcpy(p.magic, "GHST", 4);
  p.version = PROTOCOL_VERSION; p.type = uint8_t(PacketType::MESSAGE);
  p.senderRole = 1; p.networkId = 7; p.session = 15; p.frame = 1;
  p.seq = 1; p.payloadLen = 1; p.payload[0] = 'x';
  return p;
}
static void validation() {
  const auto good = validMessage();
  assert(sizeof(good) <= 250 && validPacket(good, 0, 7));
  auto bad = good; bad.senderRole = 9; assert(!validPacket(bad, 0, 7));
  bad = good; bad.type = 99; assert(!validPacket(bad, 0, 7));
  bad = good; bad.payloadLen = MAX_TEXT + 1; assert(!validPacket(bad, 0, 7));
  bad = good; bad.payload[1] = 'z'; assert(!validPacket(bad, 0, 7));
  bad = good; bad.msIntoSlot = HOP_INTERVAL_MS; assert(!validPacket(bad, 0, 7));
  bad = good; bad.session = 0; assert(!validPacket(bad, 0, 7));
  bad = good; bad.version = 2; assert(!validPacket(bad, 0, 7));
  assert(!validPacket(good, 0, 8));
  auto ack = good; ack.type = uint8_t(PacketType::ACK); ack.seq = 0;
  ack.payloadLen = 0; ack.payload[0] = 0; ack.ack = 1;
  assert(!validPacket(ack, 0, 7));
  ack.ackSession = 10; assert(validPacket(ack, 0, 7));
}

static void sessions() {
  ReplayWindow w; bool latest = false;
  assert(w.accept(44, 100, latest) && latest);
  assert(!w.accept(44, 100, latest));
  assert(w.accept(44, 102, latest) && latest);
  assert(w.accept(44, 101, latest) && !latest);
  assert(!w.accept(44, 101, latest));
  assert(w.accept(44, 140, latest));
  assert(!w.accept(44, 101, latest));
  assert(w.accept(55, 1, latest) && latest); // Peer reboot.
  assert(!w.accept(44, 141, latest));        // Retired boot cannot rewind the clock.
  ReplayWindow wrap;
  assert(wrap.accept(7, 0xfffffffeu, latest));
  assert(wrap.accept(7, 1, latest) && latest);
}

static void backpressure() {
  MessageDedup d; bool duplicate = false;
  unsigned accepted = 0;
  assert(!d.accept(10, 1, [] { return false; }, duplicate)); // Queue full: no ACK.
  assert(!d.contains(10, 1));
  assert(d.accept(10, 1, [&] { ++accepted; return true; }, duplicate) && !duplicate);
  assert(d.accept(10, 1, [] { return false; }, duplicate) && duplicate); // Lost ACK.
  assert(accepted == 1);
  assert(d.accept(11, 1, [&] { ++accepted; return true; }, duplicate) && !duplicate);
  assert(accepted == 2); // Same message number after sender reboot is new.
  Outbox q;
  for (unsigned i = 1; i <= OUTBOX_DEPTH; ++i) assert(q.push("test", 4, i));
  assert(!q.push("full", 4, 99) && q.size() == OUTBOX_DEPTH);
  assert(!q.acknowledge(99, 100, 1) && q.size() == OUTBOX_DEPTH); // Stale boot ACK.
  assert(!q.acknowledge(100, 100, 2)); // Never remove a different pending message.
  for (unsigned i = 1; i <= OUTBOX_DEPTH; ++i) assert(q.acknowledge(100, 100, i));
  assert(!q.front() && q.push("again", 5, 9));
}

// Exercise the real outbox/dedup/replay components under loss, ACK loss,
// application backpressure and a temporary total outage in BOTH directions.
static void lossyLink() {
  std::mt19937 rng(0x47484f53);
  for (uint32_t direction = 0; direction < 2; ++direction) {
    Outbox q; MessageDedup inbox; ReplayWindow replay;
    uint32_t next = 1, frame = 0, completed = 0;
    std::set<uint32_t> received;
    for (unsigned tick = 0; tick < 30000 && completed < 200; ++tick) {
      if (next <= 200 && q.push("queued offline", 14, next)) ++next;
      if (tick < 100 || (tick >= 400 && tick < 650)) continue;
      const auto* pending = q.front();
      if (!pending) continue;
      ++frame;
      if (rng() % 100 < 30) continue;
      bool latest = false;
      assert(replay.accept(700 + direction, frame, latest));
      bool duplicate = false;
      const auto seq = pending->seq;
      const bool ack = inbox.accept(700 + direction, seq, [&] {
        if (rng() % 100 < 20) return false;
        assert(received.insert(seq).second); return true;
      }, duplicate);
      if (ack && rng() % 100 >= 35) {
        assert(q.acknowledge(700 + direction, 700 + direction, seq)); ++completed;
      }
    }
    assert(completed == 200 && received.size() == 200 && q.size() == 0);
  }
}
int main() {
  clocks(); validation(); sessions(); backpressure(); lossyLink();
  std::puts("PASS: timing/wrap, packet validation, reboot/replay, queue backpressure, 400 lossy two-way deliveries");
}
