#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "ghost_config.h"

namespace ghost {
enum class PacketType : uint8_t { SYNC = 1, MESSAGE = 2, ACK = 3, PING = 4 };

struct __attribute__((packed)) GhostPacket {
  char magic[4];
  uint8_t version, type, senderRole, flags;
  uint16_t networkId;
  uint32_t session;        // Fresh random sender boot identity.
  uint32_t frame;          // Increases for every submitted frame, including retries.
  uint32_t seq;            // Logical message identity, stable across retries.
  uint32_t ack, ackSession;
  uint32_t slot;
  uint16_t msIntoSlot, payloadLen;
  char payload[MAX_TEXT + 1];
  uint8_t tag[16];
};
static_assert(sizeof(GhostPacket) <= 250, "Must fit ESP-NOW v1");

inline bool newer(uint32_t value, uint32_t previous) {
  const uint32_t delta = value - previous;
  return delta != 0 && delta < 0x80000000UL;
}
inline bool validPacket(const GhostPacket& p, uint8_t myRole, uint16_t network) {
  if (std::memcmp(p.magic, "GHST", 4) || p.version != PROTOCOL_VERSION ||
      p.networkId != network || p.senderRole != (myRole ^ 1) ||
      !p.session || !p.frame || p.flags || p.msIntoSlot >= HOP_INTERVAL_MS ||
      p.payloadLen > MAX_TEXT || p.payload[p.payloadLen] != '\0') return false;
  const auto type = static_cast<PacketType>(p.type);
  if (type == PacketType::MESSAGE)
    return p.seq && p.payloadLen && !p.ack && !p.ackSession;
  if (p.payloadLen || p.seq) return false;
  if (type == PacketType::ACK) return p.ack && p.ackSession;
  return (type == PacketType::SYNC || type == PacketType::PING) && !p.ack && !p.ackSession;
}

// Unsigned subtraction keeps timing correct over the 49-day millis wrap.
struct SlotClock {
  uint32_t slot = 0, start = 0;
  void advance(uint32_t now) {
    const uint32_t steps = (now - start) / HOP_INTERVAL_MS;
    slot += steps;
    start += steps * HOP_INTERVAL_MS;
  }
  void sync(uint32_t remoteSlot, uint16_t into, uint32_t receivedAt, uint32_t now) {
    slot = remoteSlot;
    start = receivedAt - into;
    advance(now);
  }
  uint16_t phase(uint32_t now) const { return uint16_t((now - start) % HOP_INTERVAL_MS); }
};

inline bool txWindow(uint8_t role, uint16_t phase, bool urgent) {
  if (phase < HOP_GUARD_MS || phase >= HOP_INTERVAL_MS - HOP_GUARD_MS) return false;
  if (urgent) return true;
  const auto half = HOP_INTERVAL_MS / 2;
  return role == 0 ? phase < half - HOP_GUARD_MS : phase >= half + HOP_GUARD_MS;
}

// Bounded replay protection within this boot; intentionally not persistent.
struct ReplayWindow {
  uint32_t session = 0, highest = 0, bits = 0;
  uint32_t retired[4]{};
  uint8_t retiredNext = 0;
  bool accept(uint32_t s, uint32_t frame, bool& latest) {
    latest = false;
    if (!s || !frame) return false;
    if (s != session) {
      for (const auto old : retired) if (s == old) return false;
      if (session) retired[retiredNext++ % 4] = session;
      session = s; highest = frame; bits = 1; latest = true; return true;
    }
    if (newer(frame, highest)) {
      const uint32_t gap = frame - highest;
      bits = gap >= 32 ? 1 : (bits << gap) | 1;
      highest = frame; latest = true; return true;
    }
    const uint32_t back = highest - frame;
    if (back >= 32 || (bits & (uint32_t(1) << back))) return false;
    bits |= uint32_t(1) << back;
    return true;
  }
};

struct MessageDedup {
  struct Id { uint32_t session = 0, seq = 0; } ids[MESSAGE_DEDUP_DEPTH];
  size_t next = 0;
  bool contains(uint32_t session, uint32_t seq) const {
    for (const auto& id : ids) if (id.session == session && id.seq == seq) return true;
    return false;
  }
  void remember(uint32_t session, uint32_t seq) {
    ids[next] = {session, seq};
    next = (next + 1) % MESSAGE_DEDUP_DEPTH;
  }
  template <typename Enqueue>
  bool accept(uint32_t session, uint32_t seq, Enqueue enqueue, bool& duplicate) {
    duplicate = contains(session, seq);
    if (duplicate) return true;
    if (!enqueue()) return false;
    remember(session, seq);
    return true;
  }
};

struct OutgoingMessage { uint32_t seq = 0; uint16_t length = 0; char text[MAX_TEXT + 1]{}; };
class Outbox {
 public:
  bool push(const char* text, size_t length, uint32_t seq) {
    if (!length || length > MAX_TEXT || count_ == OUTBOX_DEPTH || !seq) return false;
    auto& entry = entries_[(head_ + count_) % OUTBOX_DEPTH];
    entry = {}; entry.seq = seq; entry.length = length;
    std::memcpy(entry.text, text, length); ++count_; return true;
  }
  const OutgoingMessage* front() const { return count_ ? &entries_[head_] : nullptr; }
  bool acknowledge(uint32_t session, uint32_t localSession, uint32_t seq) {
    if (!count_ || session != localSession || entries_[head_].seq != seq) return false;
    head_ = (head_ + 1) % OUTBOX_DEPTH; --count_; return true;
  }
  size_t size() const { return count_; }
 private:
  OutgoingMessage entries_[OUTBOX_DEPTH]{};
  size_t head_ = 0, count_ = 0;
};
} // namespace ghost
