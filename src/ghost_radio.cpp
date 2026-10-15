#include "ghost_radio.h"
#include <WiFi.h>
#include <esp_system.h>
#include "ghost_pair.h"
namespace ghost {
GhostRadio Radio;
GhostRadio* GhostRadio::instance_ = nullptr;
static constexpr uint8_t BROADCAST[6] = {255,255,255,255,255,255};
static bool zero(const uint8_t* data, size_t size) { uint8_t any = 0; for (size_t i = 0; i < size; ++i) any |= data[i]; return !any; }
static String macText(const Mac& m) { char s[18]; snprintf(s, sizeof(s), "%02X:%02X:%02X:%02X:%02X:%02X", m.bytes[0],m.bytes[1],m.bytes[2],m.bytes[3],m.bytes[4],m.bytes[5]); return String(s); }
String GhostRadio::localMacString() const { return macText(local_); }
bool GhostRadio::save() {
  if (storage_.putBytes("state", &queue_.state, sizeof(queue_.state)) == sizeof(queue_.state)) return true;
  queue_.state = backup_; ++storeFailed_;
  Serial.println("[storage] write failed; queue change rolled back"); return false;
}
bool GhostRadio::load() {
  if (!storage_.begin("ghostmesh4", false)) return false;
  uint8_t input[38]{}, digest[32]{};
  memcpy(input, local_.bytes, 6); memcpy(input + 6, a_.bytes, 6); memcpy(input + 12, b_.bytes, 6);
  memcpy(input + 18, ghostpair::LMK, 16); input[34] = uint8_t(ghostpair::NETWORK_ID); input[35] = uint8_t(ghostpair::NETWORK_ID >> 8);
  input[36] = PROTOCOL_VERSION; input[37] = role_;
  if (!hmac256(ghostpair::HOP_KEY, 32, input, sizeof(input), digest)) return false;
  const auto size = storage_.getBytesLength("state");
  if (size) {
    if (size != sizeof(queue_.state) || storage_.getBytes("state", &queue_.state, size) != size || !MeshQueue::validate(queue_.state)) {
      Serial.println("[storage] incompatible/damaged queue; refusing to discard it"); return false;
    }
    if (!equalTag(queue_.state.identity, digest, 16)) {
      Serial.println("[storage] new provisioning; starting a new mesh queue"); queue_.state = {};
    }
  }
  memcpy(queue_.state.identity, digest, 16);
  backup_ = queue_.state;
  if (!save()) return false;
  for (const auto& e : queue_.state.outbox) if (e.used) messageSeq_ = std::max(messageSeq_, uint32_t(e.envelope.header.sequence));
  for (auto& born : relayBorn_) born = millis();
  return true;
}
bool GhostRadio::begin() {
  instance_ = this;
  memcpy(a_.bytes, ghostpair::NODE_A_MAC, 6); memcpy(b_.bytes, ghostpair::NODE_B_MAC, 6);
  if (!WiFi.mode(WIFI_STA)) return false;
  WiFi.setAutoReconnect(false); WiFi.disconnect(false, false); WiFi.setSleep(false);
  esp_wifi_get_mac(WIFI_IF_STA, local_.bytes);
  Serial.printf("[radio] local STA MAC: %s\n", localMacString().c_str());
  if (GHOST_PAIR_CONFIGURED != 1) { ready_ = true; Serial.println("[radio] SETUP: provision endpoints or a relay"); return true; }
  configured_ = role_ <= 2 && a_.valid() && b_.valid() && a_ != b_ && ghostpair::NETWORK_ID &&
      !zero(ghostpair::HOP_KEY, 32) && (role_ == 2 ? local_ != a_ && local_ != b_ :
      local_ == (role_ == 0 ? a_ : b_) && !zero(ghostpair::LMK, 16));
  if (!configured_) { Serial.println("[radio] wrong role/MAC or invalid provisioning"); return false; }
  if (!crypto_.begin(ghostpair::NETWORK_ID, ghostpair::HOP_KEY, ghostpair::LMK) || !load()) return false;
  do { boot_ = esp_random(); } while (!boot_);
  do { session_ = (uint64_t(esp_random()) << 32) | esp_random(); } while (!session_);
  uint8_t digest[32]{};
  if (!deriveKey(ghostpair::HOP_KEY, 32, "GHOSTLINK channels v4", digest)) return false;
  memcpy(&seed_, digest, sizeof(seed_)); home_ = digest[4] % 11 + 1;
  rxQueue_ = xQueueCreate(RX_QUEUE_DEPTH, sizeof(RxEvent));
  txQueue_ = xQueueCreate(1, sizeof(esp_now_send_status_t));
  deliveryQueue_ = xQueueCreate(OUTBOX_DEPTH, sizeof(uint32_t));
  if (!rxQueue_ || !txQueue_ || !deliveryQueue_) return false;
  if (!setChannel(home_) || esp_now_init() != ESP_OK) return false;
  uint8_t protocols = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
  if (ENABLE_WIFI_LR) protocols |= WIFI_PROTOCOL_LR;
  if (esp_wifi_set_protocol(WIFI_IF_STA, protocols) != ESP_OK ||
      esp_wifi_config_espnow_rate(WIFI_IF_STA, ENABLE_WIFI_LR ? WIFI_PHY_RATE_LORA_250K : WIFI_PHY_RATE_1M_L) != ESP_OK ||
      esp_wifi_set_max_tx_power(WIFI_MAX_TX_POWER_QDBM) != ESP_OK) return false;
  esp_wifi_get_max_tx_power(&txPower_);
  esp_now_peer_info_t peer{}; memcpy(peer.peer_addr, BROADCAST, 6); peer.channel = 0; peer.ifidx = WIFI_IF_STA;
  // ESP-NOW cannot encrypt broadcast frames. Our end-to-end AES-GCM and
  // transport HMAC protect the envelope instead; relay builds have no chat key.
  if (esp_now_add_peer(&peer) != ESP_OK || esp_now_register_recv_cb(recvThunk) != ESP_OK || esp_now_register_send_cb(sentThunk) != ESP_OK) return false;
  wifi_promiscuous_filter_t filter{}; filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
  if (ENABLE_CHANNEL_ADAPTATION && (esp_wifi_set_promiscuous_filter(&filter) != ESP_OK ||
      esp_wifi_set_promiscuous_rx_cb(sniffThunk) != ESP_OK || esp_wifi_set_promiscuous(true) != ESP_OK)) return false;
  memset(activity_, 255, sizeof(activity_));
  const auto now = millis(); activityAt_ = now; sync_.begin(role_ == 0, boot_, now);
  nextHello_ = now + esp_random() % 300; nextTx_ = now;
  ready_ = true; printStatus(); return true;
}
bool GhostRadio::setChannel(uint8_t ch) {
  if (ch < 1 || ch > 11 || busy_) return false;
  if (ch == channel_) return true;
  if (esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) != ESP_OK) return false;
  channel_ = ch; return true;
}
void GhostRadio::applyCandidate(uint8_t ch) {
  if (role_ == 0 || !ready_ || !configured_ || synced() || ch < 1 || ch > 11 || candidateChannel_) return;
  candidateChannel_ = ch; candidateUntil_ = millis() + BLE_CANDIDATE_MS;
}
void GhostRadio::updateSchedule(uint32_t now) {
  sync_.advance(now);
  if (synced()) {
    setChannel(sync_.plan.channel(sync_.clock.slot, home_, seed_));
  } else {
    if (candidateChannel_ && due(now, candidateUntil_)) candidateChannel_ = 0;
    setChannel(candidateChannel_ ? candidateChannel_ : home_);
  }
}
void GhostRadio::updateActivity(uint32_t now) {
  if (channel_) dwell_[channel_ - 1] += now - activityAt_;
  activityAt_ = now;
  portENTER_CRITICAL(&monitorMux_);
  for (unsigned i = 0; i < 11; ++i) { counts_[i] += sniffCounts_[i]; sniffCounts_[i] = 0; }
  portEXIT_CRITICAL(&monitorMux_);
  for (unsigned i = 0; i < 11; ++i) if (dwell_[i] >= 2000) {
    const uint8_t sample = std::min<uint64_t>(254, uint64_t(counts_[i]) * 1000 / dwell_[i]);
    activity_[i] = activity_[i] == 255 ? sample : (unsigned(activity_[i]) * 3 + sample) / 4;
    counts_[i] = dwell_[i] = 0;
  }
}
void GhostRadio::reviewChannels(uint32_t now) {
  if (!ENABLE_CHANNEL_ADAPTATION || role_ != 0 || now - reviewAt_ < CHANNEL_REVIEW_MS || sync_.plan.mask != sync_.plan.nextMask) return;
  reviewAt_ = now;
  uint8_t score[11]; memcpy(score, activity_, sizeof(score));
  for (const auto& n : peers_.entries) if (n.used && now - n.lastSeen < PEER_LOST_MS)
    for (unsigned i = 0; i < 11; ++i) if (n.activity[i] != 255) score[i] = score[i] == 255 ? n.activity[i] : std::max(score[i], n.activity[i]);
  const uint16_t proposed = quietChannels(score);
  if (proposed == ALL_CHANNELS || proposed == sync_.plan.mask) return;
  unsigned oldSum = 0, oldCount = 0, newSum = 0;
  for (unsigned i = 0; i < 11; ++i) {
    if (sync_.plan.mask & (1u << i)) { oldSum += score[i]; ++oldCount; }
    if (proposed & (1u << i)) newSum += score[i];
  }
  // Avoid churning schedules over tiny measurement differences.
  if (newSum * oldCount * 4 >= oldSum * 3 * 3) return;
  sync_.plan.nextMask = proposed; sync_.plan.changeSlot = sync_.clock.slot + 16;
  Serial.printf("[channels] proposed mask=%03x activation slot=%lu\n", proposed, (unsigned long)sync_.plan.changeSlot);
}
bool GhostRadio::sendFrame(FrameType type, const uint8_t* body, size_t size, uint8_t hops) {
  const auto now = millis();
  if (!ready_ || !configured_ || busy_ || !due(now, nextTx_)) return false;
  if (synced()) {
    const auto phase = sync_.clock.phase(now);
    if (phase < HOP_GUARD_MS || phase >= HOP_INTERVAL_MS - HOP_GUARD_MS || channel_ != sync_.plan.channel(sync_.clock.slot, home_, seed_)) return false;
  }
  FrameHeader h{}; h.type = uint8_t(type); h.network = ghostpair::NETWORK_ID; h.sender = local_;
  h.boot = boot_; if (++frame_ == 0) ++frame_; h.frame = frame_; h.hops = hops;
  uint8_t wire[MAX_FRAME]{}; const auto length = encodeFrame(h, body, size, crypto_, wire);
  if (!length) return false;
  busy_ = true; busyAt_ = now;
  const auto result = esp_now_send(BROADCAST, wire, length);
  nextTx_ = now + TX_SPACING_MS + esp_random() % 120;
  if (result != ESP_OK) { busy_ = false; ++txFailed_; return false; }
  ++txCount_; return true;
}
bool GhostRadio::sendEnvelope(const Envelope& e, uint8_t hops) {
  uint8_t body[MAX_BODY]{}; const auto length = packEnvelope(e, body);
  return length && sendFrame(FrameType::ENVELOPE, body, length, hops);
}
bool GhostRadio::endpoints(const Envelope& e) const {
  return (e.header.source == a_ && e.header.destination == b_) || (e.header.source == b_ && e.header.destination == a_);
}
void GhostRadio::queueAck(const Envelope& data) {
  Envelope ack{}; ack.header.type = uint8_t(MessageType::ACK);
  ack.header.source = local_; ack.header.destination = data.header.source;
  ack.header.session = data.header.session; ack.header.sequence = data.header.sequence;
  if (!crypto_.seal(ack)) return;
  for (const auto& item : ackQueue_) if (item.used && sameMessage(item.envelope, ack)) return;
  for (auto& item : ackQueue_) if (!item.used) { item = {ack, 1, 0}; return; }
  // Full receipt queue: the origin retains its data and retries.
}
void GhostRadio::process(const RxEvent& event) {
  FrameHeader h{};
  if (!decodeFrame(event.bytes, event.length, ghostpair::NETWORK_ID, event.source, crypto_, h) || h.sender == local_) { ++rejected_; return; }
  const auto* body = event.bytes + sizeof(h);
  Hello hello{}; Envelope e{};
  if (h.type == uint8_t(FrameType::HELLO)) {
    if (h.length != sizeof(Hello) || h.hops) { ++rejected_; return; }
    memcpy(&hello, body, sizeof(hello));
    if (!validHello(hello) || (hello.distance == 0 && h.sender != a_)) { ++rejected_; return; }
  } else if (!unpackEnvelope(body, h.length, e) || !endpoints(e)) { ++rejected_; return; }
  bool discovered = false;
  auto* neighbor = peers_.accept(h.sender, h.boot, h.frame, event.at, discovered);
  if (!neighbor) { ++rejected_; return; }
  ++rxCount_; rssi_ = event.rssi;
  if (discovered) {
    Serial.printf("[mesh] neighbor %s\n", macText(h.sender).c_str());
    for (auto& t : outDue_) t = event.at + esp_random() % 500;
    for (auto& t : relayDue_) t = event.at + esp_random() % 700;
    memset(relayAttempts_, 0, sizeof(relayAttempts_));
  }
  if (h.type == uint8_t(FrameType::HELLO)) {
    memcpy(neighbor->activity, hello.activity, 11);
    if (sync_.hear(hello, event.at, millis())) candidateChannel_ = 0;
    return;
  }
  if (e.header.destination == local_ && role_ != 2) {
    auto plain = e;
    if (!crypto_.open(plain)) { ++rejected_; return; }
    peerSeen_ = true; peerAt_ = event.at;
    if (e.header.type == uint8_t(MessageType::DATA)) {
      char text[MAX_TEXT + 1]{};
      if (!decodeText(plain, text)) { ++rejected_; return; }
      bool duplicate = false; backup_ = queue_.state;
      if (!queue_.receive(e, duplicate)) return;
      if (duplicate) ++duplicates_;
      else if (!save()) return;
      queueAck(e);
    } else {
      if (!uxQueueSpacesAvailable(deliveryQueue_)) return;
      backup_ = queue_.state;
      if (queue_.acknowledge(e) && save()) {
        const uint32_t sequence = e.header.sequence;
        xQueueSend(deliveryQueue_, &sequence, 0);
      }
    }
    return;
  }
  if (e.header.source == local_) return; // A flood returning to its origin.
  const int receipt = queue_.receiptFor(e);
  if (receipt >= 0) {
    relayAttempts_[receipt] = 0; relayDue_[receipt] = event.at + 80 + esp_random() % 200;
    ++duplicates_; return;
  }
  backup_ = queue_.state;
  const bool removed = queue_.clearAcknowledged(e);
  size_t index = 0; bool changed = false;
  const bool cached = queue_.cache(e, h.hops, index, changed);
  if ((changed || removed) && !save()) return;
  if (cached && changed) { relayAttempts_[index] = 0; relayBorn_[index] = event.at; relayDue_[index] = event.at + 80 + esp_random() % 500; }
  else if (cached) ++duplicates_;
}
bool GhostRadio::sendMessage(const String& text) {
  if (!ready_ || !configured_ || role_ == 2 || messageSeq_ == UINT32_MAX) return false;
  Envelope e{}; e.header.type = uint8_t(MessageType::DATA); e.header.source = local_;
  e.header.destination = role_ == 0 ? b_ : a_; e.header.session = session_; e.header.sequence = messageSeq_ + 1;
  if (!encodeText(text.c_str(), text.length(), e) || !crypto_.seal(e)) return false;
  backup_ = queue_.state;
  if (!queue_.enqueue(e) || !save()) return false;
  messageSeq_ = lastQueuedSeq_ = e.header.sequence;
  for (size_t i = 0; i < OUTBOX_DEPTH; ++i) if (queue_.state.outbox[i].used && sameMessage(queue_.state.outbox[i].envelope, e)) outDue_[i] = millis();
  return true;
}
bool GhostRadio::restoredMessage(size_t index, String& text, uint32_t& seq) const {
  if (index >= OUTBOX_DEPTH || !queue_.state.outbox[index].used) return false;
  auto e = queue_.state.outbox[index].envelope; char decoded[MAX_TEXT + 1]{};
  if (!crypto_.open(e) || !decodeText(e, decoded)) return false;
  text = decoded; seq = e.header.sequence; return true;
}
bool GhostRadio::popReceivedMessage(String& text, uint32_t& seq) {
  for (auto& item : queue_.state.inbox) if (item.used) {
    auto e = item.envelope; char decoded[MAX_TEXT + 1]{};
    if (!crypto_.open(e) || !decodeText(e, decoded)) { ready_ = false; return false; }
    backup_ = queue_.state; item = {};
    if (!save()) return false;
    text = decoded; seq = e.header.sequence; return true;
  }
  return false;
}
bool GhostRadio::consumeDeliveryNotice(uint32_t& seq, bool& success) {
  success = true; return deliveryQueue_ && xQueueReceive(deliveryQueue_, &seq, 0) == pdTRUE;
}
void GhostRadio::loop() {
  if (!ready_ || !configured_) return;
  esp_now_send_status_t status;
  while (xQueueReceive(txQueue_, &status, 0) == pdTRUE) { busy_ = false; if (status != ESP_NOW_SEND_SUCCESS) ++txFailed_; }
  auto now = millis();
  if (busy_ && now - busyAt_ > 2000) { ready_ = false; Serial.println("[radio] missing send callback; restart board"); return; }
  RxEvent event{};
  for (unsigned i = 0; i < RX_QUEUE_DEPTH && xQueueReceive(rxQueue_, &event, 0) == pdTRUE; ++i) process(event);
  now = millis();
  updateActivity(now); updateSchedule(now); reviewChannels(now);
  for (auto& ack : ackQueue_) if (ack.used && sendEnvelope(ack.envelope, 0)) { ack = {}; break; }
  if (due(now, nextHello_)) {
    auto hello = sync_.advertisement(millis());
    memcpy(hello.activity, activity_, 11);
    if (sendFrame(FrameType::HELLO, reinterpret_cast<const uint8_t*>(&hello), sizeof(hello))) {
      sync_.sent(hello);
      nextHello_ = now + HELLO_INTERVAL_MS + esp_random() % 500;
    }
  }
  if (neighbors()) {
    for (size_t n = 0; n < OUTBOX_DEPTH + RELAY_DEPTH; ++n) {
      const size_t index = (sendCursor_ + n) % (OUTBOX_DEPTH + RELAY_DEPTH);
      const bool outgoing = index < OUTBOX_DEPTH;
      const size_t i = outgoing ? index : index - OUTBOX_DEPTH;
      auto& item = outgoing ? queue_.state.outbox[i] : queue_.state.relays[i];
      auto& deadline = outgoing ? outDue_[i] : relayDue_[i];
      if (!outgoing && item.envelope.header.type == uint8_t(MessageType::ACK) && relayAttempts_[i] >= 3) continue;
      if (item.used && due(now, deadline) && sendEnvelope(item.envelope, item.hops)) {
        deadline = now + (outgoing ? ACK_TIMEOUT_MS : RELAY_RETRY_MS) + esp_random() % 1000;
        if (!outgoing && relayAttempts_[i] < 255) ++relayAttempts_[i];
        sendCursor_ = (index + 1) % (OUTBOX_DEPTH + RELAY_DEPTH); break;
      }
    }
  }
  if (now - expiryAt_ >= 1000) {
    expiryAt_ = now; backup_ = queue_.state; bool changed = false;
    for (size_t i = 0; i < RELAY_DEPTH; ++i) {
      auto& item = queue_.state.relays[i];
      const auto ttl = item.envelope.header.type == uint8_t(MessageType::ACK) ? ACK_RETENTION_MS : RELAY_RETENTION_MS;
      if (item.used && now - relayBorn_[i] >= ttl) { item = {}; changed = true; }
    }
    if (changed) save();
  }
}
void GhostRadio::onReceive(const uint8_t* mac, const uint8_t* data, int len, int8_t rssi) {
  if (!ready_ || !mac || !data || len < int(sizeof(FrameHeader) + 16) || len > int(MAX_FRAME)) return;
  RxEvent event{}; memcpy(event.bytes, data, len); memcpy(event.source.bytes, mac, 6);
  event.length = len; event.at = millis(); event.rssi = rssi; xQueueSend(rxQueue_, &event, 0);
}
void GhostRadio::recvThunk(
#if ESP_IDF_VERSION_MAJOR >= 5
    const esp_now_recv_info_t* info,
#else
    const uint8_t* mac,
#endif
    const uint8_t* data, int len) {
  if (!instance_) return;
#if ESP_IDF_VERSION_MAJOR >= 5
  instance_->onReceive(info ? info->src_addr : nullptr, data, len, info && info->rx_ctrl ? info->rx_ctrl->rssi : -127);
#else
  instance_->onReceive(mac, data, len, -127);
#endif
}
void GhostRadio::sentThunk(const uint8_t*, esp_now_send_status_t status) { if (instance_ && instance_->txQueue_) xQueueOverwrite(instance_->txQueue_, &status); }
void GhostRadio::sniffThunk(void* buffer, wifi_promiscuous_pkt_type_t type) {
  if (!instance_ || !buffer || type != WIFI_PKT_MGMT) return;
  const auto* packet = static_cast<const wifi_promiscuous_pkt_t*>(buffer);
  // Only beacon/probe-response activity; exclude our ESP-NOW action frames.
  if (packet->rx_ctrl.sig_len < 24) return;
  const uint8_t subtype = packet->payload[0] & 0xfc;
  const uint8_t ch = packet->rx_ctrl.channel;
  if ((subtype != 0x80 && subtype != 0x50) || ch < 1 || ch > 11) return;
  portENTER_CRITICAL(&instance_->monitorMux_);
  if (instance_->sniffCounts_[ch - 1] < 1000000) ++instance_->sniffCounts_[ch - 1];
  portEXIT_CRITICAL(&instance_->monitorMux_);
}
void GhostRadio::printStatus() const {
  Serial.printf("[status] v%s role=%c ready=%u configured=%u neighbors=%u route_recent=%u sync=%u ch=%u home=%u distance=%u queue=%u relay=%u tx=%lu rx=%lu rejected=%lu duplicates=%lu txfail=%lu storefail=%lu\n",
      VERSION, role_ == 2 ? 'R' : char('A' + role_), ready_, configured_, unsigned(neighbors()), linked(), synced(), channel_, home_, sync_.distance, unsigned(queue_.queued()), unsigned(queue_.relayed()),
      (unsigned long)txCount_, (unsigned long)rxCount_, (unsigned long)rejected_, (unsigned long)duplicates_, (unsigned long)txFailed_, (unsigned long)storeFailed_);
  Serial.printf("[channels] mask=%03x next=%03x switch=%lu activity=", sync_.plan.mask, sync_.plan.nextMask, (unsigned long)sync_.plan.changeSlot);
  for (auto v : activity_) Serial.printf("%u,", v);
  Serial.printf(" txcap=%.2f dBm\n", txPower_ / 4.0f);
}
void GhostRadio::printNeighbors() const {
  const auto now = millis();
  for (const auto& n : peers_.entries) if (n.used && now - n.lastSeen < PEER_LOST_MS)
    Serial.printf("[neighbor] %s age=%lu ms\n", macText(n.mac).c_str(), (unsigned long)(now - n.lastSeen));
}
} // namespace ghost
