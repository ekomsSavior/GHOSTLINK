#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "ghost_config.h"
namespace ghost {
struct __attribute__((packed)) Mac {
  uint8_t bytes[6]{};
  bool operator==(const Mac& b) const { return std::memcmp(bytes, b.bytes, 6) == 0; }
  bool operator!=(const Mac& b) const { return !(*this == b); }
  bool valid() const { uint8_t any = 0; for (auto b : bytes) any |= b; return any && !(bytes[0] & 1); }
};
inline bool newer(uint32_t a, uint32_t b) { return a != b && uint32_t(a - b) < 0x80000000u; }
inline bool due(uint32_t now, uint32_t deadline) { return int32_t(now - deadline) >= 0; }
struct SlotClock {
  uint32_t slot = 0, start = 0;
  void advance(uint32_t now) { const uint32_t steps = (now - start) / HOP_INTERVAL_MS; slot += steps; start += steps * HOP_INTERVAL_MS; }
  void sync(uint32_t s, uint16_t phase, uint32_t at, uint32_t now) { slot = s; start = at - phase; advance(now); }
  uint16_t phase(uint32_t now) const { return (now - start) % HOP_INTERVAL_MS; }
};
struct ReplayWindow {
  uint32_t session = 0, highest = 0, bits = 0, retired[4]{};
  uint8_t next = 0;
  bool accept(uint32_t s, uint32_t frame, bool& latest) {
    latest = false;
    if (!s || !frame) return false;
    if (s != session) {
      for (auto old : retired) if (old == s) return false;
      if (session) retired[next++ % 4] = session;
      session = s; highest = frame; bits = 1; latest = true; return true;
    }
    if (newer(frame, highest)) {
      const uint32_t gap = frame - highest;
      bits = gap >= 32 ? 1 : (bits << gap) | 1;
      highest = frame; latest = true; return true;
    }
    const uint32_t back = highest - frame;
    if (back >= 32 || (bits & (uint32_t(1) << back))) return false;
    bits |= uint32_t(1) << back; return true;
  }
};
enum class FrameType : uint8_t { HELLO = 1, ENVELOPE = 2 };
enum class MessageType : uint8_t { DATA = 1, ACK = 2 };
struct __attribute__((packed)) EnvelopeHeader {
  uint8_t type = 0, codec = 0, plainLength = 0, length = 0;
  Mac source{}, destination{};
  uint64_t session = 0;
  uint32_t sequence = 0;
};
struct Envelope { EnvelopeHeader header{}; uint8_t data[MAX_TEXT]{}, tag[16]{}; };
struct __attribute__((packed)) FrameHeader {
  char magic[4]; uint8_t version, type; uint16_t network; Mac sender;
  uint32_t boot, frame; uint8_t hops, length;
};
constexpr size_t MAX_BODY = sizeof(EnvelopeHeader) + MAX_TEXT + 16;
constexpr size_t MAX_FRAME = sizeof(FrameHeader) + MAX_BODY + 16;
static_assert(sizeof(EnvelopeHeader) == 28 && MAX_FRAME <= 250, "ESP-NOW v1 frame budget");
inline bool validEnvelope(const Envelope& e) {
  const auto& h = e.header;
  if (!h.source.valid() || !h.destination.valid() || h.source == h.destination || !h.session || !h.sequence || h.length > MAX_TEXT || h.plainLength > MAX_TEXT) return false;
  if (h.type == uint8_t(MessageType::ACK)) return !h.length && !h.plainLength && !h.codec;
  if (h.type != uint8_t(MessageType::DATA) || !h.plainLength) return false;
  return h.codec == 0 ? h.length == h.plainLength : h.codec == 1 && h.length == (h.plainLength * 6 + 7) / 8 && h.length < h.plainLength;
}
inline size_t packEnvelope(const Envelope& e, uint8_t* out) {
  if (!validEnvelope(e)) return 0;
  std::memcpy(out, &e.header, sizeof(e.header));
  std::memcpy(out + sizeof(e.header), e.data, e.header.length);
  std::memcpy(out + sizeof(e.header) + e.header.length, e.tag, 16);
  return sizeof(e.header) + e.header.length + 16;
}
inline bool unpackEnvelope(const uint8_t* data, size_t size, Envelope& e) {
  if (size < sizeof(EnvelopeHeader) + 16) return false;
  e = {}; std::memcpy(&e.header, data, sizeof(e.header));
  if (!validEnvelope(e) || size != sizeof(e.header) + e.header.length + 16) return false;
  std::memcpy(e.data, data + sizeof(e.header), e.header.length);
  std::memcpy(e.tag, data + sizeof(e.header) + e.header.length, 16); return true;
}
inline bool sameMessage(const Envelope& a, const Envelope& b) {
  return a.header.type == b.header.type && a.header.source == b.header.source && a.header.destination == b.header.destination && a.header.session == b.header.session && a.header.sequence == b.header.sequence;
}
inline bool acknowledges(const Envelope& ack, const Envelope& data) {
  return ack.header.type == uint8_t(MessageType::ACK) && data.header.type == uint8_t(MessageType::DATA) && ack.header.source == data.header.destination && ack.header.destination == data.header.source && ack.header.session == data.header.session && ack.header.sequence == data.header.sequence;
}
// Lossless 6-bit packing reduces actual frame length, not RF path loss.
constexpr char ALPHABET[] = " abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.";
static_assert(sizeof(ALPHABET) == 65, "Six-bit alphabet");
inline bool encodeText(const char* text, size_t length, Envelope& e) {
  if (!length || length > MAX_TEXT || std::memchr(text, 0, length)) return false;
  bool fits = (length * 6 + 7) / 8 < length;
  for (size_t i = 0; i < length && fits; ++i) fits = std::strchr(ALPHABET, text[i]) != nullptr;
  e.header.plainLength = length; e.header.codec = fits ? 1 : 0;
  e.header.length = fits ? (length * 6 + 7) / 8 : length;
  std::memset(e.data, 0, sizeof(e.data));
  if (!fits) { std::memcpy(e.data, text, length); return true; }
  for (size_t i = 0; i < length; ++i) {
    const uint8_t value = std::strchr(ALPHABET, text[i]) - ALPHABET;
    for (unsigned bit = 0; bit < 6; ++bit) if (value & (1u << bit)) e.data[(i * 6 + bit) / 8] |= 1u << ((i * 6 + bit) % 8);
  }
  return true;
}
inline bool decodeText(const Envelope& e, char out[MAX_TEXT + 1]) {
  if (!validEnvelope(e) || e.header.type != uint8_t(MessageType::DATA)) return false;
  if (!e.header.codec) {
    if (std::memchr(e.data, 0, e.header.length)) return false;
    std::memcpy(out, e.data, e.header.length);
  } else {
    for (size_t i = 0; i < e.header.plainLength; ++i) {
      uint8_t value = 0;
      for (unsigned bit = 0; bit < 6; ++bit) value |= ((e.data[(i * 6 + bit) / 8] >> ((i * 6 + bit) % 8)) & 1u) << bit;
      out[i] = ALPHABET[value];
    }
    const unsigned used = (e.header.plainLength * 6) % 8;
    if (used && (e.data[e.header.length - 1] >> used)) return false;
  }
  out[e.header.plainLength] = 0; return true;
}
struct __attribute__((packed)) Hello {
  uint32_t rootBoot = 0, pulse = 0, slot = 0;
  uint16_t phase = 0, mask = ALL_CHANNELS, nextMask = ALL_CHANNELS;
  uint32_t changeSlot = 0;
  uint8_t distance = 255;
  uint8_t activity[11]{}; // 255 = not sampled; otherwise observed Wi-Fi frames/second.
};
inline bool validHello(const Hello& h) {
  return h.phase < HOP_INTERVAL_MS && h.mask && !(h.mask & ~ALL_CHANNELS) && h.nextMask && !(h.nextMask & ~ALL_CHANNELS) && (h.distance == 255 || (h.distance <= MAX_HOPS && h.rootBoot && h.pulse));
}
struct ChannelPlan {
  uint16_t mask = ALL_CHANNELS, nextMask = ALL_CHANNELS;
  uint32_t changeSlot = 0;
  void advance(uint32_t slot) { if (mask != nextMask && (slot == changeSlot || newer(slot, changeSlot))) mask = nextMask; }
  uint8_t channel(uint32_t slot, uint8_t home, uint32_t seed) const {
    if (slot % RENDEZVOUS_EVERY_SLOTS == 0) return home;
    if (slot % 4 == 0) return uint8_t((slot / 4) % 11 + 1); // Explore unused channels.
    uint8_t choices[11]{}, count = 0;
    for (unsigned i = 0; i < 11; ++i) if (mask & (1u << i)) choices[count++] = i + 1;
    if (!count) return home;
    uint32_t x = slot ^ seed; x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return choices[x % count];
  }
};
inline uint16_t quietChannels(const uint8_t activity[11]) {
  for (unsigned i = 0; i < 11; ++i) if (activity[i] == 255) return ALL_CHANNELS;
  uint16_t mask = 0;
  for (unsigned n = 0; n < 3; ++n) {
    unsigned best = 11;
    for (unsigned i = 0; i < 11; ++i) if (!(mask & (1u << i)) && (best == 11 || activity[i] < activity[best])) best = i;
    mask |= 1u << best;
  }
  return mask;
}
} // namespace ghost
