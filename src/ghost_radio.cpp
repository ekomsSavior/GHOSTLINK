#include "ghost_radio.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <mbedtls/md.h>
#include <algorithm>
#include "ghost_pair.h"

namespace ghost {
GhostRadio Radio;
GhostRadio* GhostRadio::instance_ = nullptr;

static bool allZero(const uint8_t* bytes, size_t length) {
  uint8_t any = 0;
  for (size_t i = 0; i < length; ++i) any |= bytes[i];
  return any == 0;
}
static String macString(const uint8_t* mac) {
  char text[18];
  snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(text);
}
const uint8_t* GhostRadio::peerMac() const { return role_ == 0 ? ghostpair::NODE_B_MAC : ghostpair::NODE_A_MAC; }
String GhostRadio::peerMacString() const { return macString(peerMac()); }
String GhostRadio::localMacString() const {
  uint8_t mac[6]{}; esp_wifi_get_mac(WIFI_IF_STA, mac); return macString(mac);
}

bool GhostRadio::configurePhy() {
  const uint8_t protocol = ENABLE_WIFI_LR ? (LR_ONLY_PHY ? WIFI_PROTOCOL_LR :
      WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR) :
      WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
  if (esp_wifi_set_protocol(WIFI_IF_STA, protocol) != ESP_OK) return false;
  // Select the transmit rate as well as the receive protocol. Never silently
  // fall back on only one of the pair.
  const auto rate = ENABLE_WIFI_LR ? WIFI_PHY_RATE_LORA_250K : WIFI_PHY_RATE_1M_L;
  if (esp_wifi_config_espnow_rate(WIFI_IF_STA, rate) != ESP_OK) return false;
  lrEnabled_ = ENABLE_WIFI_LR;
  if (esp_wifi_set_max_tx_power(WIFI_MAX_TX_POWER_QDBM) != ESP_OK) return false;
  esp_wifi_get_max_tx_power(&txPowerQdbm_);
  Serial.printf("[radio] %s, TX power cap %.2f dBm\n", lrEnabled_ ? "LR 250K" : "1M", txPowerQdbm_ / 4.0f);
  return true;
}

bool GhostRadio::begin() {
  instance_ = this;
  if (!WiFi.mode(WIFI_STA)) return false;
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  WiFi.setSleep(false);
  delay(40);
  Serial.printf("[radio] local STA MAC: %s\n", localMacString().c_str());

  if (GHOST_PAIR_CONFIGURED != 1) {
    Serial.println("[radio] SETUP: generate include/ghost_pair_private.h with both MACs");
    ready_ = true;
    return true;
  }
  uint8_t local[6]{}; esp_wifi_get_mac(WIFI_IF_STA, local);
  const auto expected = role_ == 0 ? ghostpair::NODE_A_MAC : ghostpair::NODE_B_MAC;
  configured_ = ghostpair::NETWORK_ID && !allZero(peerMac(), 6) && !(peerMac()[0] & 1) &&
      memcmp(local, expected, 6) == 0 && memcmp(local, peerMac(), 6) != 0 &&
      !allZero(ghostpair::PMK, 16) && !allZero(ghostpair::LMK, 16) && !allZero(ghostpair::HOP_KEY, 32);
  if (!configured_) {
    Serial.println("[radio] invalid pair keys/MACs or wrong A/B build for this board");
    return false;
  }
  rxQueue_ = xQueueCreate(RX_QUEUE_DEPTH, sizeof(RxEvent));
  messageQueue_ = xQueueCreate(MESSAGE_QUEUE_DEPTH, sizeof(GhostPacket));
  txQueue_ = xQueueCreate(2, sizeof(esp_now_send_status_t));
  deliveryQueue_ = xQueueCreate(OUTBOX_DEPTH, sizeof(uint32_t));
  if (!rxQueue_ || !messageQueue_ || !txQueue_ || !deliveryQueue_) return false;
  if (!setChannel(1) || esp_now_init() != ESP_OK || !configurePhy() ||
      esp_now_set_pmk(ghostpair::PMK) != ESP_OK) return false;
  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, peerMac(), 6);
  memcpy(peer.lmk, ghostpair::LMK, 16);
  peer.channel = 0; peer.ifidx = WIFI_IF_STA; peer.encrypt = true;
  if (esp_now_add_peer(&peer) != ESP_OK || esp_now_register_recv_cb(recvThunk) != ESP_OK ||
      esp_now_register_send_cb(sentThunk) != ESP_OK) return false;
  do { session_ = esp_random(); } while (!session_);
  const auto now = millis();
  clock_.start = now;
  ready_ = true;
  if (role_ == 0) { synced_ = true; setChannel(channelForSlot(0)); }
  else enterHunt(now);
  printStatus();
  return ready_;
}

uint8_t GhostRadio::channelForSlot(uint32_t slot) const {
  if (slot % RENDEZVOUS_EVERY_SLOTS == 0) slot = 0;
  uint8_t data[6] = {uint8_t(ghostpair::NETWORK_ID >> 8), uint8_t(ghostpair::NETWORK_ID),
      uint8_t(slot >> 24), uint8_t(slot >> 16), uint8_t(slot >> 8), uint8_t(slot)};
  uint8_t digest[32]{};
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), ghostpair::HOP_KEY, 32, data, sizeof(data), digest);
  return digest[0] % 11 + 1;
}
uint8_t GhostRadio::rendezvousChannel() const { return channelForSlot(0); }
bool GhostRadio::setChannel(uint8_t ch) {
  if (ch < 1 || ch > 11 || txBusy_) return false;
  if (ch == currentChannel_) return true;
  if (esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) != ESP_OK) return false;
  currentChannel_ = ch; return true;
}
bool GhostRadio::linked() const {
  return ready_ && configured_ && peerSeen_ && synced() && ageSincePeerMs() < PEER_LOST_MS;
}

void GhostRadio::signPacket(GhostPacket& packet) const {
  uint8_t key[32]{}, digest[32]{};
  constexpr uint8_t domain[] = "GHOSTLINK packet v3";
  const auto* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_hmac(md, ghostpair::HOP_KEY, 32, domain, sizeof(domain) - 1, key);
  mbedtls_md_hmac(md, key, sizeof(key), reinterpret_cast<const uint8_t*>(&packet), offsetof(GhostPacket, tag), digest);
  memcpy(packet.tag, digest, sizeof(packet.tag));
}
bool GhostRadio::verifyPacket(const GhostPacket& packet) const {
  auto expected = packet; signPacket(expected);
  uint8_t diff = 0;
  for (size_t i = 0; i < sizeof(packet.tag); ++i) diff |= packet.tag[i] ^ expected.tag[i];
  return diff == 0;
}
GhostPacket GhostRadio::makePacket(PacketType type) const {
  GhostPacket p{}; memcpy(p.magic, "GHST", 4);
  p.version = PROTOCOL_VERSION; p.type = uint8_t(type); p.senderRole = role_;
  p.networkId = ghostpair::NETWORK_ID; p.session = session_;
  return p;
}
bool GhostRadio::sendPacket(GhostPacket& p, bool urgent) {
  const auto now = millis();
  if (!ready_ || !synced() || txBusy_ || now - lastTxMs_ < TX_SPACING_MS ||
      currentChannel_ != channelForSlot(clock_.slot) || !txWindow(role_, clock_.phase(now), urgent)) return false;
  p.slot = clock_.slot; p.msIntoSlot = clock_.phase(now);
  if (++frame_ == 0) ++frame_;
  p.frame = frame_; signPacket(p);
  txBusy_ = true;
  const auto err = esp_now_send(peerMac(), reinterpret_cast<const uint8_t*>(&p), sizeof(p));
  if (err != ESP_OK) { txBusy_ = false; ++txFailed_; return false; }
  ++txCount_; lastTxMs_ = now; return true;
}

void GhostRadio::enterHunt(uint32_t now) {
  synced_ = false; candidate_ = false;
  huntStartedMs_ = now; huntSinceMs_ = now;
  huntChannel_ = currentChannel_ ? currentChannel_ : rendezvousChannel();
}
void GhostRadio::updateChannel(uint32_t now) {
  if (role_ == 1 && synced_ && ageSincePeerMs() >= PEER_LOST_MS) enterHunt(now);
  if (role_ == 0 || synced_) {
    clock_.advance(now);
    if (!txBusy_) setChannel(channelForSlot(clock_.slot));
    return;
  }
  if (candidate_ && now - candidateSinceMs_ < BLE_CANDIDATE_MS) {
    setChannel(candidateChannel_); return;
  }
  candidate_ = false;
  if (now - huntStartedMs_ >= DEEP_HUNT_AFTER_MS) {
    setChannel(rendezvousChannel()); return;
  }
  if (now - huntSinceMs_ >= HUNT_DWELL_MS + (session_ % 47)) {
    huntChannel_ = huntChannel_ % 11 + 1;
    huntSinceMs_ = now;
  }
  setChannel(huntChannel_);
}
void GhostRadio::applyExternalSync(uint32_t slot, uint16_t phase) {
  if (role_ != 1 || !ready_ || !configured_ || synced_ || phase >= HOP_INTERVAL_MS || candidate_) return;
  // BLE suggests a channel; only authenticated ESP-NOW establishes sync/liveness.
  candidateChannel_ = channelForSlot(slot);
  candidateSinceMs_ = millis(); candidate_ = true;
}

bool GhostRadio::sendMessage(const String& text) {
  if (!configured_ || !ready_ || !text.length() || text.length() > MAX_TEXT) return false;
  uint32_t next = messageSeq_ + 1; if (!next) ++next;
  if (!outbox_.push(text.c_str(), text.length(), next)) return false;
  messageSeq_ = lastQueuedSeq_ = next;
  return true;
}
bool GhostRadio::popReceivedMessage(String& text, uint32_t& seq) {
  GhostPacket packet{};
  if (!messageQueue_ || xQueueReceive(messageQueue_, &packet, 0) != pdTRUE) return false;
  text = String(packet.payload); seq = packet.seq; return true;
}
bool GhostRadio::consumeDeliveryNotice(uint32_t& seq, bool& success) {
  if (!deliveryQueue_ || xQueueReceive(deliveryQueue_, &seq, 0) != pdTRUE) return false;
  success = true; return true;
}

void GhostRadio::processPacket(const RxEvent& event) {
  const auto& p = event.packet;
  bool latest = false;
  if (!validPacket(p, role_, ghostpair::NETWORK_ID) || !verifyPacket(p) ||
      !replay_.accept(p.session, p.frame, latest)) { ++rejectedCount_; return; }
  ++rxCount_;
  if (latest) {
    peerSeen_ = true; lastPeerRxMs_ = event.receivedAt; lastRssi_ = event.rssi;
    if (role_ == 1) {
      clock_.sync(p.slot, p.msIntoSlot, event.receivedAt, millis());
      synced_ = true; candidate_ = false;
    }
  }
  switch (static_cast<PacketType>(p.type)) {
    case PacketType::MESSAGE: {
      bool duplicate = false;
      // No ACK until the application receive queue accepts a new message.
      if (!dedup_.accept(p.session, p.seq, [&] { return xQueueSend(messageQueue_, &p, 0) == pdTRUE; }, duplicate)) return;
      if (duplicate) ++duplicateCount_;
      ackSeq_ = p.seq; ackSession_ = p.session; ackPending_ = true;
      break;
    }
    case PacketType::ACK: {
      const auto* front = outbox_.front();
      if (front && p.ackSession == session_ && front->seq == p.ack &&
          uxQueueSpacesAvailable(deliveryQueue_)) {
        const uint32_t delivered = p.ack;
        outbox_.acknowledge(p.ackSession, session_, p.ack);
        xQueueSend(deliveryQueue_, &delivered, 0);
        attempts_ = 0; retryRest_ = false;
      }
      break;
    }
    default: break;
  }
}

void GhostRadio::loop() {
  if (!ready_ || !configured_) return;
  esp_now_send_status_t status;
  while (xQueueReceive(txQueue_, &status, 0) == pdTRUE) {
    txBusy_ = false;
    if (status != ESP_NOW_SEND_SUCCESS) ++txFailed_;
  }
  RxEvent event{};
  while (xQueueReceive(rxQueue_, &event, 0) == pdTRUE) processPacket(event);
  const auto now = millis();
  updateChannel(now);
  if (!synced()) return;

  if (ackPending_) {
    auto packet = makePacket(PacketType::ACK);
    packet.ack = ackSeq_; packet.ackSession = ackSession_;
    if (sendPacket(packet, true)) ackPending_ = false;
  }
  if (role_ == 0 && now - lastSyncTxMs_ >= SYNC_INTERVAL_MS) {
    auto packet = makePacket(PacketType::SYNC);
    if (sendPacket(packet, true)) lastSyncTxMs_ = now;
  }
  const auto* front = outbox_.front();
  if (front && linked()) {
    if (retryRest_ && now - retryRestStartedMs_ >= RETRY_REST_MS) { retryRest_ = false; attempts_ = 0; }
    if (!retryRest_ && (!attempts_ || now - retryStartedMs_ >= ACK_TIMEOUT_MS)) {
      if (attempts_ >= MAX_RETRIES) { retryRest_ = true; retryRestStartedMs_ = now; }
      else {
        auto packet = makePacket(PacketType::MESSAGE);
        packet.seq = front->seq; packet.payloadLen = front->length;
        memcpy(packet.payload, front->text, front->length + 1);
        if (sendPacket(packet, false)) { ++attempts_; retryStartedMs_ = now; }
      }
    }
  }
  if (role_ == 1 && now - lastPingMs_ >= PING_INTERVAL_MS) {
    auto packet = makePacket(PacketType::PING);
    if (sendPacket(packet, false)) lastPingMs_ = now;
  }
}

void GhostRadio::onReceive(const uint8_t* mac, const uint8_t* data, int len, int8_t rssi) {
  if (!ready_ || !mac || !data || memcmp(mac, peerMac(), 6) || len != int(sizeof(GhostPacket))) return;
  RxEvent event{};
  memcpy(&event.packet, data, sizeof(GhostPacket));
  event.receivedAt = millis(); event.rssi = rssi;
  xQueueSend(rxQueue_, &event, 0);
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
void GhostRadio::sentThunk(const uint8_t*, esp_now_send_status_t status) {
  if (instance_ && instance_->txQueue_) xQueueSend(instance_->txQueue_, &status, 0);
}
void GhostRadio::printStatus() const {
  Serial.printf("[status] v%s role=%c ready=%u paired=%u link=%u sync=%u ch=%u slot=%lu outbox=%u tx=%lu txfail=%lu rx=%lu duplicate=%lu rejected=%lu\n",
      VERSION, role_ ? 'B' : 'A', ready_, configured_, linked(), synced(), currentChannel_,
      (unsigned long)clock_.slot, unsigned(outbox_.size()), (unsigned long)txCount_,
      (unsigned long)txFailed_, (unsigned long)rxCount_, (unsigned long)duplicateCount_, (unsigned long)rejectedCount_);
}
} // namespace ghost
