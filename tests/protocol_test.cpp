#include "ghost_crypto.h"
#include "ghost_mesh.h"
#include <cassert>
#include <cstdio>
#include <random>
#include <set>
#include <string>
#include <vector>
using namespace ghost;
static Mac mac(uint8_t id) { return Mac{{2,0,0,0,0,id}}; }
static constexpr uint8_t HOP[32] = {1,2,3,4,5,6,7,8};
static constexpr uint8_t CHAT[16] = {9,8,7,6,5,4,3,2};
static MeshCrypto crypto(bool relay = false) {
  MeshCrypto c; const uint8_t empty[16]{}; assert(c.begin(42, HOP, relay ? empty : CHAT)); return c;
}
static Envelope message(uint32_t seq = 1, uint8_t from = 1, uint8_t to = 2, const std::string& text = "hello across the mesh") {
  Envelope e{}; e.header.type = uint8_t(MessageType::DATA); e.header.source = mac(from); e.header.destination = mac(to);
  e.header.session = uint64_t(from) * 10000000000ull; e.header.sequence = seq;
  assert(encodeText(text.data(), text.size(), e)); assert(crypto().seal(e)); return e;
}
static Envelope ackFor(const Envelope& e) {
  Envelope ack{}; ack.header.type = uint8_t(MessageType::ACK); ack.header.source = e.header.destination; ack.header.destination = e.header.source;
  ack.header.session = e.header.session; ack.header.sequence = e.header.sequence; assert(crypto().seal(ack)); return ack;
}
static void codecAndCrypto() {
  std::mt19937 rng(42);
  for (unsigned length = 1; length <= MAX_TEXT; ++length) {
    for (unsigned kind = 0; kind < 3; ++kind) {
      std::string text(length, ' ');
      for (auto& ch : text) ch = kind == 0 ? ALPHABET[rng() % 64] : kind == 1 ? char(1 + rng() % 255) : 'a';
      auto e = message(length, 1, 2, text); const auto encrypted = e;
      uint8_t wire[MAX_BODY]{}; const auto n = packEnvelope(e, wire); Envelope recovered{};
      assert(n == sizeof(EnvelopeHeader) + e.header.length + 16);
      assert(unpackEnvelope(wire, n, recovered) && sameMessage(e, recovered));
      assert(!unpackEnvelope(wire, n - 1, recovered));
      assert(!crypto(true).open(e)); // A relay has transport membership but no chat key.
      e = encrypted; assert(crypto().open(e)); char result[MAX_TEXT + 1]{};
      assert(decodeText(e, result) && std::string(result, length) == text);
      auto bad = encrypted; bad.tag[0] ^= 1; assert(!crypto().open(bad));
      bad = encrypted; bad.data[0] ^= 1; assert(!crypto().open(bad));
      bad = encrypted; bad.header.destination = mac(4); assert(!crypto().open(bad));
      bad = encrypted; ++bad.header.sequence; assert(!crypto().open(bad));
      if (kind == 0 && length >= 4) assert(encrypted.header.length < length);
    }
  }
  auto e = message(); auto ack = ackFor(e);
  assert(crypto().open(ack) && acknowledges(ack, e));
  auto forged = ack; ++forged.header.session; assert(!crypto().open(forged));
  assert(!crypto(true).open(ack));
  uint8_t key[32]{}, nonce[12]{}, tag[16]{}, data[16]{};
  // NIST AES-256-GCM zero-key vector, 16 zero plaintext bytes, no AAD.
  const uint8_t ciphertext[16] = {0xce,0xa7,0x40,0x3d,0x4d,0x60,0x6b,0x6e,0x07,0x4e,0xc5,0xd3,0xba,0xf3,0x9d,0x18};
  const uint8_t expectedTag[16] = {0xd0,0xd1,0xc8,0xa7,0x99,0x99,0x6b,0xf0,0x26,0x5b,0x98,0xb5,0xd4,0x8a,0xb9,0x19};
  assert(gcm(true, key, nonce, nullptr, 0, data, sizeof(data), tag));
  assert(!memcmp(data, ciphertext, 16) && !memcmp(tag, expectedTag, 16));
}
static void frameValidation() {
  const auto e = message(); uint8_t body[MAX_BODY]{}, wire[MAX_FRAME]{};
  const auto length = packEnvelope(e, body); FrameHeader h{};
  h.type = uint8_t(FrameType::ENVELOPE); h.network = 42; h.sender = mac(1); h.boot = 17; h.frame = 1;
  const auto size = encodeFrame(h, body, length, crypto(), wire); assert(size && size <= 250);
  FrameHeader parsed{};
  assert(decodeFrame(wire, size, 42, mac(1), crypto(true), parsed));
  assert(!decodeFrame(wire, size, 41, mac(1), crypto(), parsed));
  assert(!decodeFrame(wire, size, 42, mac(9), crypto(), parsed));
  for (size_t i = 0; i < size; ++i) { wire[i] ^= 1; assert(!decodeFrame(wire, size, 42, mac(1), crypto(), parsed)); wire[i] ^= 1; }
  for (size_t n = 0; n < size; ++n) assert(!decodeFrame(wire, n, 42, mac(1), crypto(), parsed));
  h.hops = MAX_HOPS; assert(!encodeFrame(h, body, length, crypto(), wire));
  // Random malformed frames/envelopes are safe to parse.
  std::mt19937 rng(19);
  for (unsigned n = 0; n < 10000; ++n) {
    for (auto& b : wire) b = rng();
    decodeFrame(wire, rng() % (MAX_FRAME + 1), 42, mac(1), crypto(), parsed);
    Envelope fuzz{}; unpackEnvelope(wire, rng() % (MAX_FRAME + 1), fuzz);
  }
}
static void timingAndDiscovery() {
  SlotClock c; c.start = 0xfffffff0u; c.slot = 17;
  const uint32_t now = c.start + HOP_INTERVAL_MS + 123; c.advance(now);
  assert(c.slot == 18 && c.phase(now) == 123);
  c.sync(90, 2950, 5000, 5250); assert(c.slot == 91 && c.phase(5250) == 200);
  ChannelPlan p; p.nextMask = 0x41; p.changeSlot = 16;
  p.advance(15); assert(p.mask == ALL_CHANNELS); p.advance(16); assert(p.mask == 0x41);
  for (uint32_t slot = 0; slot < 10000; ++slot) {
    const auto ch = p.channel(slot, 11, 123);
    assert(ch >= 1 && ch <= 11);
    if (slot % 8 == 0) assert(ch == 11);
    else if (slot % 4) assert(p.mask & (1u << (ch - 1)));
  }
  uint8_t score[11] = {40,30,20,10,9,8,7,6,5,4,3};
  assert(quietChannels(score) == 0x700); score[2] = 255; assert(quietChannels(score) == ALL_CHANNELS);
  NeighborTable peers; bool added = false;
  assert(peers.accept(mac(1), 1, 1, 100, added) && added && peers.count(100) == 1);
  assert(!peers.accept(mac(1), 1, 1, 100, added));
  assert(peers.count(100 + PEER_LOST_MS) == 0);
  assert(peers.accept(mac(1), 2, 1, 20000, added) && added);
  assert(!peers.accept(mac(1), 1, 2, 20001, added));
  ReplayWindow replay; bool latest;
  assert(replay.accept(1, 0xfffffffe, latest)); assert(replay.accept(1, 1, latest) && latest);
  assert(replay.accept(1, 0xffffffff, latest) && !latest); assert(!replay.accept(1, 0xffffffff, latest));
}
static void meshSynchronization() {
  MeshClock a, c, b;
  a.begin(true, 71, 1000); c.begin(false, 81, 5000); b.begin(false, 91, 9000);
  // Different local uptime origins and receive-queue delays across two hops.
  a.advance(1100); auto h = a.advertisement(1100); a.sent(h);
  assert(c.hear(h, 5100, 5250));
  assert(c.distance == 1 && c.clock.phase(5250) == 250);
  auto forwarded = c.advertisement(5250);
  assert(b.hear(forwarded, 9250, 9350));
  assert(b.distance == 2 && b.clock.phase(9350) == 350);
  // The same pulse recirculating around a mesh must not renew the clock lease.
  assert(!c.hear(b.advertisement(9350), 5400, 5400));
  c.advance(5100 + PEER_LOST_MS); assert(!c.synchronized);
  b.advance(9250 + PEER_LOST_MS); assert(!b.synchronized);
  // Resume with a fresh pulse and a future channel-plan activation.
  a.advance(14000); a.plan.nextMask = 0x421; a.plan.changeSlot = a.clock.slot + 16;
  h = a.advertisement(14000); a.sent(h);
  assert(c.hear(h, 18000, 18000)); assert(b.hear(c.advertisement(18000), 22000, 22000));
  for (unsigned dt = 0; dt < 6000; dt += 100) {
    a.advance(14000 + dt); c.advance(18000 + dt); b.advance(22000 + dt);
    assert(a.plan.channel(a.clock.slot, 6, 123) == b.plan.channel(b.clock.slot, 6, 123));
  }
  // Anchor reboot can establish a new clock; its retired boot cannot rewind it.
  MeshClock restarted; restarted.begin(true, 72, 0); restarted.advance(100);
  auto fresh = restarted.advertisement(100); restarted.sent(fresh);
  assert(c.hear(fresh, 25000, 25000));
  ++h.pulse; assert(!c.hear(h, 25100, 25100));
  auto malformed = fresh; malformed.mask = 0; assert(!validHello(malformed));
  malformed = fresh; malformed.distance = MAX_HOPS; assert(!b.hear(malformed, 100, 100));
  malformed = fresh; malformed.distance = MAX_HOPS - 1;
  MeshClock tail; tail.begin(false, 20, 0); assert(tail.hear(malformed, 100, 100));
  assert(tail.distance == MAX_HOPS && validHello(tail.advertisement(100)));
}
static void channelRecoverySimulation() {
  std::mt19937 rng(700);
  MeshClock nodes[3];
  const uint32_t offsets[3] = {1000, 9000, 47000};
  uint32_t deadlines[3] = {300, 900, 1500};
  for (unsigned i = 0; i < 3; ++i) nodes[i].begin(i == 0, 70 + i, offsets[i]);
  bool established = false, recovered = false, rebooted = false;
  for (uint32_t t = 0; t < 240000; t += 50) {
    if (t == 30000) { nodes[0].plan.nextMask = 0x421; nodes[0].plan.changeSlot = nodes[0].clock.slot + 16; }
    if (t == 150000) { nodes[0] = {}; nodes[0].begin(true, 99, t + offsets[0]); }
    for (unsigned i = 0; i < 3; ++i) nodes[i].advance(t + offsets[i]);
    for (unsigned i = 0; i < 3; ++i) {
      if (!due(t, deadlines[i])) continue;
      const auto phase = nodes[i].clock.phase(t + offsets[i]);
      if (nodes[i].synchronized && (phase < HOP_GUARD_MS || phase >= HOP_INTERVAL_MS - HOP_GUARD_MS)) continue;
      const auto h = nodes[i].advertisement(t + offsets[i]); nodes[i].sent(h);
      deadlines[i] = t + HELLO_INTERVAL_MS + rng() % 500;
      const auto channel = nodes[i].synchronized ? nodes[i].plan.channel(nodes[i].clock.slot, 6, 123) : 6;
      for (unsigned j = 0; j < 3; ++j) {
        if ((i + 1 != j && j + 1 != i) || rng() % 100 < 30) continue;
        if (j == 2 && t >= 40000 && t < 110000) continue; // Miss plan activation entirely.
        const auto receivingChannel = nodes[j].synchronized ? nodes[j].plan.channel(nodes[j].clock.slot, 6, 123) : 6;
        if (channel == receivingChannel) nodes[j].hear(h, t + offsets[j], t + offsets[j]);
      }
    }
    if (t < 30000 && nodes[2].synchronized) established = true;
    if (t > 110000 && t < 150000 && nodes[2].synchronized && nodes[2].plan.mask == 0x421) recovered = true;
    if (t > 150000 && nodes[2].synchronized && nodes[2].rootBoot == 99) rebooted = true;
  }
  assert(established && recovered && rebooted);
  std::puts("PASS: three-node channel rendezvous, missed plan activation, anchor reboot and 30% HELLO loss");
}
static void storageAndBounds() {
  MeshQueue q; bool duplicate = false;
  for (unsigned i = 1; i <= OUTBOX_DEPTH; ++i) assert(q.enqueue(message(i)));
  assert(!q.enqueue(message(99)));
  auto badAck = ackFor(message(1)); ++badAck.header.session; assert(!q.acknowledge(badAck));
  assert(q.acknowledge(ackFor(message(1))) && q.queued() == OUTBOX_DEPTH - 1);
  for (unsigned i = 1; i <= MESSAGE_QUEUE_DEPTH; ++i) assert(q.receive(message(i), duplicate) && !duplicate);
  assert(!q.receive(message(99), duplicate) && !q.seen(message(99)));
  assert(q.receive(message(1), duplicate) && duplicate);
  // A serialized snapshot restores queue and duplicate receipts after reboot.
  MeshState persisted{}; memcpy(&persisted, &q.state, sizeof(persisted));
  MeshQueue reboot; reboot.state = persisted;
  assert(MeshQueue::validate(reboot.state) && reboot.queued() == q.queued());
  assert(reboot.receive(message(1), duplicate) && duplicate);
  auto corrupt = persisted; corrupt.nextReceipt = 255; assert(!MeshQueue::validate(corrupt));
  size_t index = 0; bool changed = false;
  for (unsigned i = 0; i < RELAY_DEPTH; ++i) assert(q.cache(message(i + 1), 0, index, changed) && changed);
  assert(!q.cache(message(99), 0, index, changed));
  assert(q.cache(ackFor(message()), 0, index, changed) && changed);
  assert(q.receiptFor(message()) >= 0);
  assert(!q.cache(message(900), MAX_HOPS - 1, index, changed));
  MeshQueue loop; assert(loop.cache(message(), 2, index, changed) && changed && loop.state.relays[index].hops == 3);
  assert(loop.cache(message(), 4, index, changed) && !changed); // Longer loop cannot reset state.
  assert(loop.cache(message(), 0, index, changed) && changed && loop.state.relays[index].hops == 1);
  auto poison = message(); poison.data[0] ^= 1; assert(!loop.cache(poison, 0, index, changed));
}
struct SimNode {
  uint8_t id; MeshCrypto keys; MeshQueue queue; NeighborTable peers;
  uint32_t frame = 0, boot = 1; std::vector<Envelope> acks;
  std::set<std::pair<uint64_t,uint32_t>> displayed;
  SimNode(uint8_t n) : id(n), keys(crypto(n > 2)) {}
  void receive(const std::vector<uint8_t>& wire, uint8_t from, uint32_t now) {
    FrameHeader h{}; if (!decodeFrame(wire.data(), wire.size(), 42, mac(from), keys, h)) return;
    bool discovered = false; if (!peers.accept(h.sender, h.boot, h.frame, now, discovered)) return;
    Envelope e{}; if (!unpackEnvelope(wire.data() + sizeof(h), h.length, e)) return;
    if (e.header.destination == mac(id)) {
      auto decoded = e; assert(keys.open(decoded));
      if (e.header.type == uint8_t(MessageType::ACK)) { queue.acknowledge(e); return; }
      bool duplicate = false;
      if (queue.receive(e, duplicate)) {
        if (!duplicate) assert(displayed.insert({uint64_t(e.header.session), uint32_t(e.header.sequence)}).second);
        acks.push_back(ackFor(e));
      }
    } else if (e.header.source != mac(id)) {
      const int receipt = queue.receiptFor(e);
      if (receipt >= 0) return;
      queue.clearAcknowledged(e);
      size_t index = 0; bool changed = false; queue.cache(e, h.hops, index, changed);
    }
  }
  std::vector<std::vector<uint8_t>> transmissions() {
    std::vector<std::vector<uint8_t>> result;
    auto send = [&](const Envelope& e, uint8_t hops) {
      uint8_t body[MAX_BODY]{}, wire[MAX_FRAME]{}; auto len = packEnvelope(e, body);
      FrameHeader h{}; h.type = uint8_t(FrameType::ENVELOPE); h.network = 42; h.sender = mac(id); h.boot = boot; h.frame = ++frame; h.hops = hops;
      auto size = encodeFrame(h, body, len, keys, wire); assert(size); result.emplace_back(wire, wire + size);
    };
    for (const auto& e : queue.state.outbox) if (e.used) send(e.envelope, 0);
    for (const auto& e : queue.state.relays) if (e.used) send(e.envelope, e.hops);
    for (const auto& e : acks) send(e, 0);
    acks.clear();
    for (auto& e : queue.state.inbox) e = {}; // Application has consumed the durable inbox.
    return result;
  }
};
static void meshPaths() {
  unsigned deliveries = 0;
  for (unsigned trial = 0; trial < 50; ++trial) {
    std::vector<SimNode> nodes; for (auto id : {1,2,3,4}) nodes.emplace_back(id);
    assert(nodes[0].queue.enqueue(message(trial + 1)));
    assert(nodes[1].queue.enqueue(message(trial + 1, 2, 1)));
    std::mt19937 rng(123 + trial);
    for (unsigned tick = 0; tick < 500 && (nodes[0].queue.queued() || nodes[1].queue.queued()); ++tick) {
      if (tick == 30) { // Relay power cycle retains ciphertext but loses volatile discovery state.
        auto saved = nodes[2].queue.state; nodes[2] = SimNode(3); nodes[2].boot = 2; nodes[2].queue.state = saved;
      }
      for (size_t i = 0; i < nodes.size(); ++i) {
        auto batch = nodes[i].transmissions();
        for (const auto& packet : batch) for (size_t j = 0; j < nodes.size(); ++j) {
          bool adjacent = false;
          // No endpoint-to-endpoint edge ever exists. First C carries across a
          // partition, then D supplies a different path after C disappears.
          if (tick < 20) adjacent = (i == 0 && j == 2) || (i == 2 && j == 0);
          else if (tick < 60) adjacent = (i == 1 && j == 2) || (i == 2 && j == 1);
          else adjacent = (i == 3 && j < 2) || (j == 3 && i < 2);
          if (tick >= 90 && tick < 120) adjacent = false; // Complete outage.
          if (adjacent && rng() % 100 >= 40) nodes[j].receive(packet, nodes[i].id, tick * 100);
        }
      }
    }
    assert(!nodes[0].queue.queued() && !nodes[1].queue.queued());
    assert(nodes[0].displayed.size() == 1 && nodes[1].displayed.size() == 1);
    deliveries += 2;
  }
  std::printf("PASS: %u bidirectional mesh deliveries through relays, partitions, relay restart, route replacement and 40%% packet loss\n", deliveries);
}
int main() {
  codecAndCrypto(); frameValidation(); timingAndDiscovery(); meshSynchronization(); channelRecoverySimulation(); storageAndBounds(); meshPaths();
  std::puts("PASS: compression, AES-GCM vector, tamper rejection, relay key separation, malformed frames, wrap, discovery, persistence format and bounded queues/hops");
}
