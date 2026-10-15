#pragma once
#include "ghost_protocol.h"
namespace ghost {
struct StoredEnvelope { Envelope envelope{}; uint8_t used = 0, hops = 0; };
struct ReceiptId {
  Mac source{}; uint64_t session = 0; uint32_t sequence = 0;
  bool matches(const Envelope& e) const { return source == e.header.source && session == e.header.session && sequence == e.header.sequence; }
};
// One atomic NVS blob contains the durable queues and recent receive receipts.
struct MeshState {
  uint32_t format = 4;
  uint8_t identity[16]{};
  StoredEnvelope outbox[OUTBOX_DEPTH]{}, relays[RELAY_DEPTH]{}, inbox[MESSAGE_QUEUE_DEPTH]{};
  ReceiptId receipts[MESSAGE_DEDUP_DEPTH]{};
  uint8_t nextReceipt = 0;
};
class MeshQueue {
 public:
  MeshState state{};
  bool seen(const Envelope& e) const { for (const auto& id : state.receipts) if (id.matches(e)) return true; return false; }
  bool enqueue(const Envelope& e) { return add(state.outbox, OUTBOX_DEPTH, e, 0); }
  bool receive(const Envelope& e, bool& duplicate) {
    duplicate = seen(e);
    if (duplicate) return true;
    if (!add(state.inbox, MESSAGE_QUEUE_DEPTH, e, 0)) return false;
    auto& id = state.receipts[state.nextReceipt];
    id.source = e.header.source; id.session = e.header.session; id.sequence = e.header.sequence;
    state.nextReceipt = (state.nextReceipt + 1) % MESSAGE_DEDUP_DEPTH;
    return true;
  }
  bool acknowledge(const Envelope& e) {
    for (auto& item : state.outbox) if (item.used && acknowledges(e, item.envelope)) { item = {}; return true; }
    return false;
  }
  bool cache(const Envelope& e, uint8_t hops, size_t& index, bool& changed) {
    changed = false;
    if (!validEnvelope(e) || hops + 1 >= MAX_HOPS) return false;
    for (size_t i = 0; i < RELAY_DEPTH; ++i) {
      auto& item = state.relays[i];
      if (item.used && sameMessage(item.envelope, e)) {
        index = i;
        // Identical IDs with different contents never replace cached ciphertext.
        if (std::memcmp(&item.envelope, &e, sizeof(e))) return false;
        if (hops + 1 < item.hops) { item.hops = hops + 1; changed = true; }
        return true;
      }
    }
    for (size_t i = 0; i < RELAY_DEPTH; ++i) if (!state.relays[i].used) {
      state.relays[i] = {e, 1, uint8_t(hops + 1)}; index = i; changed = true; return true;
    }
    // A receipt can displace data, preventing a full data cache from blocking ACKs.
    if (e.header.type == uint8_t(MessageType::ACK)) {
      for (size_t i = 0; i < RELAY_DEPTH; ++i) if (state.relays[i].envelope.header.type == uint8_t(MessageType::DATA)) {
        state.relays[i] = {e, 1, uint8_t(hops + 1)}; index = i; changed = true; return true;
      }
    }
    return false;
  }
  int receiptFor(const Envelope& data) const {
    for (size_t i = 0; i < RELAY_DEPTH; ++i)
      if (state.relays[i].used && acknowledges(state.relays[i].envelope, data)) return int(i);
    return -1;
  }
  bool clearAcknowledged(const Envelope& ack) {
    bool changed = false;
    for (auto& item : state.relays) if (item.used && acknowledges(ack, item.envelope)) { item = {}; changed = true; }
    return changed;
  }
  size_t queued() const { return count(state.outbox, OUTBOX_DEPTH); }
  size_t relayed() const { return count(state.relays, RELAY_DEPTH); }
  static bool validate(const MeshState& s) {
    if (s.format != 4 || s.nextReceipt >= MESSAGE_DEDUP_DEPTH) return false;
    for (const auto& e : s.outbox) if (e.used && (e.used != 1 || e.hops || !validEnvelope(e.envelope) || e.envelope.header.type != uint8_t(MessageType::DATA))) return false;
    for (const auto& e : s.relays) if (e.used && (e.used != 1 || e.hops >= MAX_HOPS || !validEnvelope(e.envelope))) return false;
    for (const auto& e : s.inbox) if (e.used && (e.used != 1 || !validEnvelope(e.envelope) || e.envelope.header.type != uint8_t(MessageType::DATA))) return false;
    return true;
  }
 private:
  static size_t count(const StoredEnvelope* entries, size_t size) { size_t n = 0; for (size_t i = 0; i < size; ++i) n += entries[i].used != 0; return n; }
  static bool add(StoredEnvelope* entries, size_t size, const Envelope& e, uint8_t hops) {
    if (!validEnvelope(e)) return false;
    for (size_t i = 0; i < size; ++i) if (!entries[i].used) { entries[i] = {e, 1, hops}; return true; }
    return false;
  }
};
struct Neighbor {
  Mac mac{}; ReplayWindow replay{};
  uint32_t lastSeen = 0;
  bool used = false;
  uint8_t activity[11]{};
};
class NeighborTable {
 public:
  Neighbor entries[MAX_NEIGHBORS]{};
  Neighbor* accept(const Mac& mac, uint32_t boot, uint32_t frame, uint32_t now, bool& discovered) {
    discovered = false;
    Neighbor* target = nullptr;
    for (auto& n : entries) if (n.used && n.mac == mac) { target = &n; break; }
    if (!target) {
      for (auto& n : entries) if (!n.used || now - n.lastSeen >= PEER_LOST_MS) { target = &n; break; }
      if (!target) return nullptr;
      *target = {}; target->used = true; target->mac = mac;
      std::memset(target->activity, 255, sizeof(target->activity)); discovered = true;
    } else discovered = now - target->lastSeen >= PEER_LOST_MS || target->replay.session != boot;
    bool latest = false;
    if (!target->replay.accept(boot, frame, latest)) return nullptr;
    if (latest) target->lastSeen = now;
    return target;
  }
  size_t count(uint32_t now) const { size_t n = 0; for (const auto& e : entries) n += e.used && now - e.lastSeen < PEER_LOST_MS; return n; }
};
// Only new anchor pulses refresh synchronization. Echoes of one old pulse
// cannot keep a disconnected cluster falsely synchronized forever.
class MeshClock {
 public:
  SlotClock clock;
  ChannelPlan plan;
  bool anchor = false, synchronized = false;
  uint32_t rootBoot = 0, pulse = 0, lastSync = 0;
  uint8_t distance = 255;
  void begin(bool isAnchor, uint32_t boot, uint32_t now) {
    anchor = isAnchor; synchronized = isAnchor; clock.start = now;
    if (anchor) { rootBoot = boot; distance = 0; }
  }
  void advance(uint32_t now) {
    if (!anchor && synchronized && now - lastSync >= PEER_LOST_MS) { synchronized = false; distance = 255; }
    if (synchronized) { clock.advance(now); plan.advance(clock.slot); }
  }
  bool hear(const Hello& h, uint32_t receivedAt, uint32_t now) {
    if (anchor || !validHello(h) || h.distance >= MAX_HOPS) return false;
    bool latest = false;
    if (!replay_.accept(h.rootBoot, h.pulse, latest) || !latest) return false;
    rootBoot = h.rootBoot; pulse = h.pulse; distance = h.distance + 1;
    clock.sync(h.slot, h.phase, receivedAt, now);
    plan = {h.mask, h.nextMask, h.changeSlot}; plan.advance(clock.slot);
    synchronized = true; lastSync = receivedAt; return true;
  }
  Hello advertisement(uint32_t now) const {
    Hello h{};
    if (synchronized) {
      h.rootBoot = rootBoot; h.pulse = anchor ? pulse + 1 : pulse;
      if (!h.pulse) h.pulse = 1;
      h.slot = clock.slot; h.phase = clock.phase(now);
      h.mask = plan.mask; h.nextMask = plan.nextMask; h.changeSlot = plan.changeSlot; h.distance = distance;
    }
    return h;
  }
  void sent(const Hello& h) { if (anchor) pulse = h.pulse; }
 private:
  ReplayWindow replay_;
};
} // namespace ghost
