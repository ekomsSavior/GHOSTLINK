#include "ghost_radio.h"

#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>
#include <mbedtls/md.h>
#include <cstring>

#include "ghost_pair.h"

namespace ghost {

GhostRadio Radio;
GhostRadio* GhostRadio::instance_ = nullptr;

static bool allZeroMac(const uint8_t* m) {
  for (int i = 0; i < 6; ++i) if (m[i] != 0) return false;
  return true;
}

const uint8_t* GhostRadio::peerMac() const {
  return role_ == 0 ? ghostpair::NODE_B_MAC : ghostpair::NODE_A_MAC;
}

bool GhostRadio::srcIsPeer(const uint8_t* src) const {
  return src && memcmp(src, peerMac(), 6) == 0;
}

String GhostRadio::localMacString() const {
  uint8_t mac[6] = {0};
  esp_wifi_get_mac(WIFI_IF_STA, mac);
  char b[18];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(b);
}

String GhostRadio::peerMacString() const {
  const uint8_t* mac = peerMac();
  char b[18];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(b);
}

void GhostRadio::configurePhy() {
  lrEnabled_ = false;

  if (ENABLE_WIFI_LR) {
    uint8_t protocol = WIFI_PROTOCOL_LR;
    if (!LR_ONLY_PHY) {
      protocol = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR;
    }

    const esp_err_t pe = esp_wifi_set_protocol(WIFI_IF_STA, protocol);
    if (pe == ESP_OK) {
      lrEnabled_ = true;
      Serial.printf("[ghost] GHOST-LR PHY enabled (%s)\n", LR_ONLY_PHY ? "LR-only" : "BGN+LR");
    } else {
      Serial.printf("[ghost] LR PHY unavailable (%d); using normal Wi-Fi PHY\n", int(pe));
    }
  }

  // Set a legal maximum request; the driver/chip/country configuration can
  // clamp this lower. Read back the effective driver setting for telemetry.
  const esp_err_t te = esp_wifi_set_max_tx_power(WIFI_MAX_TX_POWER_QDBM);
  if (te != ESP_OK) {
    Serial.printf("[ghost] set TX power failed (%d)\n", int(te));
  }
  int8_t actual = 0;
  if (esp_wifi_get_max_tx_power(&actual) == ESP_OK) {
    txPowerQdbm_ = actual;
    Serial.printf("[ghost] Wi-Fi max TX power: %.2f dBm\n", actual / 4.0f);
  }
}

bool GhostRadio::begin() {
  instance_ = this;

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, true); // clear saved AP state but keep the radio on
  WiFi.setSleep(false);         // lower latency for the hopping state machine
  delay(40);
  configurePhy();

  rxQueue_ = xQueueCreate(RX_QUEUE_DEPTH, sizeof(RxEvent));
  messageQueue_ = xQueueCreate(4, sizeof(GhostPacket));
  if (!rxQueue_ || !messageQueue_) return false;

  configured_ = (GHOST_PAIR_CONFIGURED == 1) && !allZeroMac(peerMac());
  if (!configured_) {
    Serial.println("[ghost] pair configuration missing; radio disabled");
    Serial.printf("[ghost] local STA MAC: %s\n", localMacString().c_str());
    return true;
  }

  setChannel(1);
  if (esp_now_init() != ESP_OK) {
    Serial.println("[ghost] esp_now_init failed");
    return false;
  }

  if (esp_now_set_pmk(ghostpair::PMK) != ESP_OK) {
    Serial.println("[ghost] esp_now_set_pmk failed");
    return false;
  }

  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, peerMac(), 6);
  memcpy(peer.lmk, ghostpair::LMK, 16);
  peer.channel = 0; // follow the local radio's current channel
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = true;

  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("[ghost] esp_now_add_peer failed");
    return false;
  }

  esp_now_register_recv_cb(recvThunk);

  const uint32_t now = millis();
  lastPeerRxMs_ = now;
  slotStartMs_ = now;
  lastHopMs_ = now;
  currentSlot_ = 1;

  if (role_ == 0) {
    synced_ = true;
    setChannel(channelForSlot(currentSlot_));
  } else {
    enterHunt();
  }

  Serial.printf("[ghost] role=%c local=%s peer=%s\n",
                role_ == 0 ? 'A' : 'B',
                localMacString().c_str(), peerMacString().c_str());
  return true;
}

uint8_t GhostRadio::channelForSlot(uint32_t slotValue) const {
  uint8_t digest[32] = {0};
  uint8_t input[6];
  input[0] = uint8_t(ghostpair::NETWORK_ID >> 8);
  input[1] = uint8_t(ghostpair::NETWORK_ID & 0xff);
  input[2] = uint8_t(slotValue >> 24);
  input[3] = uint8_t(slotValue >> 16);
  input[4] = uint8_t(slotValue >> 8);
  input[5] = uint8_t(slotValue);

  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_hmac(info, ghostpair::HOP_KEY, sizeof(ghostpair::HOP_KEY),
                  input, sizeof(input), digest);

  return uint8_t((digest[0] % 11) + 1); // US 2.4 GHz channels 1..11
}

void GhostRadio::setChannel(uint8_t ch) {
  if (ch < 1 || ch > 11) return;
  if (currentChannel_ == ch) return;
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  currentChannel_ = ch;
}


uint16_t GhostRadio::msIntoSlot() const {
  const uint32_t elapsed = millis() - slotStartMs_;
  return uint16_t(min<uint32_t>(elapsed, HOP_INTERVAL_MS - 1));
}

uint32_t GhostRadio::ageSincePeerMs() const {
  return millis() - lastPeerRxMs_;
}

bool GhostRadio::linked() const {
  if (!configured_) return false;
  if (role_ == 1 && !synced_) return false;
  return ageSincePeerMs() < PEER_LOST_MS;
}

GhostPacket GhostRadio::makePacket(PacketType type) {
  GhostPacket p{};
  memcpy(p.magic, "GHST", 4);
  p.version = PROTOCOL_VERSION;
  p.type = static_cast<uint8_t>(type);
  p.senderRole = role_;
  p.slot = currentSlot_;
  const uint32_t into = millis() - slotStartMs_;
  p.msIntoSlot = uint16_t(min<uint32_t>(into, HOP_INTERVAL_MS - 1));
  return p;
}

bool GhostRadio::sendPacket(GhostPacket& p) {
  if (!configured_) return false;
  const esp_err_t e = esp_now_send(peerMac(), reinterpret_cast<uint8_t*>(&p), sizeof(p));
  return e == ESP_OK;
}

void GhostRadio::sendSync(uint32_t now) {
  GhostPacket p = makePacket(PacketType::SYNC);
  sendPacket(p);
  lastSyncTxMs_ = now;
}

void GhostRadio::sendPing() {
  GhostPacket p = makePacket(PacketType::PING);
  sendPacket(p);
  lastPingMs_ = millis();
}

void GhostRadio::sendAck(uint32_t seq) {
  GhostPacket p = makePacket(PacketType::ACK);
  p.ack = seq;
  sendPacket(p);
}

bool GhostRadio::sendMessage(const String& text) {
  if (!configured_ || text.length() == 0 || pendingActive_) return false;

  pending_ = makePacket(PacketType::MESSAGE);
  pending_.seq = txSeq_++;
  pending_.payloadLen = min<size_t>(text.length(), MAX_TEXT);
  memcpy(pending_.payload, text.c_str(), pending_.payloadLen);
  pending_.payload[pending_.payloadLen] = '\0';

  pendingActive_ = true;
  pendingRetries_ = 0;
  pendingLastTxMs_ = 0; // force immediate send in loop
  return true;
}

bool GhostRadio::popReceivedMessage(String& outText, uint32_t& seq) {
  if (!messageQueue_) return false;
  GhostPacket p{};
  if (xQueueReceive(messageQueue_, &p, 0) != pdTRUE) return false;
  outText = String(p.payload).substring(0, p.payloadLen);
  seq = p.seq;
  return true;
}

bool GhostRadio::consumeDeliveryNotice(uint32_t& seq, bool& success) {
  if (!deliveryNotice_) return false;
  seq = deliverySeq_;
  success = deliverySuccess_;
  deliveryNotice_ = false;
  return true;
}

void GhostRadio::applyExternalSync(uint32_t slotValue, uint16_t msIntoSlot) {
  if (role_ != 1 || !configured_) return;
  currentSlot_ = slotValue;
  const uint32_t now = millis();
  const uint16_t bounded = min<uint16_t>(msIntoSlot, uint16_t(HOP_INTERVAL_MS - 1));
  slotStartMs_ = now - bounded;
  synced_ = true;
  lastPeerRxMs_ = now;
  setChannel(channelForSlot(currentSlot_));
}

void GhostRadio::leaderLoop(uint32_t now) {
  if (now - slotStartMs_ >= HOP_INTERVAL_MS) {
    const uint32_t steps = (now - slotStartMs_) / HOP_INTERVAL_MS;
    currentSlot_ += steps;
    slotStartMs_ += steps * HOP_INTERVAL_MS;
    setChannel(channelForSlot(currentSlot_));
  }

  if (now - lastSyncTxMs_ >= SYNC_INTERVAL_MS) sendSync(now);
}

void GhostRadio::enterHunt() {
  synced_ = false;
  huntChannel_ = 1;
  huntSinceMs_ = millis();
  huntStartedMs_ = huntSinceMs_;
  setChannel(huntChannel_);
}

void GhostRadio::hopFollower(uint32_t now) {
  const uint32_t lostFor = now - huntStartedMs_;
  const uint32_t dwell = lostFor >= DEEP_HUNT_AFTER_MS ? DEEP_HUNT_DWELL_MS : HUNT_DWELL_MS;
  if (now - huntSinceMs_ < dwell) return;
  huntSinceMs_ = now;

  // Sweep every legal 2.4 GHz channel. The pattern deliberately walks rather
  // than randomizes so recovery time is bounded even at the edge of range.
  huntChannel_ = uint8_t((huntChannel_ % 11) + 1);
  setChannel(huntChannel_);
}

void GhostRadio::followerLoop(uint32_t now) {
  if (!synced_) {
    hopFollower(now);
    return;
  }

  if (now - lastPeerRxMs_ >= PEER_LOST_MS) {
    enterHunt();
    return;
  }

  if (now - slotStartMs_ >= HOP_INTERVAL_MS) {
    const uint32_t steps = (now - slotStartMs_) / HOP_INTERVAL_MS;
    currentSlot_ += steps;
    slotStartMs_ += steps * HOP_INTERVAL_MS;
    setChannel(channelForSlot(currentSlot_));
  }
}

void GhostRadio::loop() {
  if (!configured_) return;

  const uint32_t now = millis();
  role_ == 0 ? leaderLoop(now) : followerLoop(now);
  drainRx();

  if (synced() && now - lastPingMs_ >= PING_INTERVAL_MS) sendPing();

  if (pendingActive_ && synced() &&
      (pendingLastTxMs_ == 0 || now - pendingLastTxMs_ >= ACK_TIMEOUT_MS)) {
    if (pendingRetries_ >= MAX_RETRIES) {
      deliveryNotice_ = true;
      deliverySeq_ = pending_.seq;
      deliverySuccess_ = false;
      pendingActive_ = false;
    } else {
      // Refresh slot metadata on every retry.
      pending_.slot = currentSlot_;
      pending_.msIntoSlot = uint16_t(min<uint32_t>(now - slotStartMs_, HOP_INTERVAL_MS - 1));
      sendPacket(pending_);
      pendingLastTxMs_ = now;
      ++pendingRetries_;
    }
  }
}

void GhostRadio::drainRx() {
  RxEvent ev{};
  while (xQueueReceive(rxQueue_, &ev, 0) == pdTRUE) processPacket(ev);
}

void GhostRadio::processPacket(const RxEvent& ev) {
  const GhostPacket& p = ev.packet;
  lastPeerRxMs_ = millis();
  lastRssi_ = ev.rssi;

  if (role_ == 1 && static_cast<PacketType>(p.type) == PacketType::SYNC) {
    currentSlot_ = p.slot;
    slotStartMs_ = millis() - min<uint16_t>(p.msIntoSlot, uint16_t(HOP_INTERVAL_MS - 1));
    synced_ = true;
    setChannel(channelForSlot(currentSlot_));
  }

  switch (static_cast<PacketType>(p.type)) {
    case PacketType::MESSAGE:
      if (p.seq != lastRxMessageSeq_) {
        GhostPacket copy = p;
        if (copy.payloadLen > MAX_TEXT) copy.payloadLen = MAX_TEXT;
        copy.payload[copy.payloadLen] = '\0';
        xQueueSend(messageQueue_, &copy, 0);
        lastRxMessageSeq_ = p.seq;
      }
      sendAck(p.seq); // ACK duplicates too; sender may have missed the first ACK.
      break;

    case PacketType::ACK:
      if (pendingActive_ && p.ack == pending_.seq) {
        deliveryNotice_ = true;
        deliverySeq_ = pending_.seq;
        deliverySuccess_ = true;
        pendingActive_ = false;
      }
      break;

    case PacketType::SYNC:
    case PacketType::PING:
    default:
      break;
  }
}

void GhostRadio::onReceive(const uint8_t* src, const uint8_t* data, int len, int8_t rssi) {
  if (!configured_ || !srcIsPeer(src) || len != int(sizeof(GhostPacket))) return;

  RxEvent ev{};
  memcpy(&ev.packet, data, sizeof(GhostPacket));
  ev.rssi = rssi;

  if (memcmp(ev.packet.magic, "GHST", 4) != 0) return;
  if (ev.packet.version != PROTOCOL_VERSION) return;
  if (ev.packet.senderRole == role_) return;
  if (ev.packet.payloadLen > MAX_TEXT) return;

  xQueueSend(rxQueue_, &ev, 0);
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
  const uint8_t* src = info ? info->src_addr : nullptr;
  const int8_t rssi = (info && info->rx_ctrl) ? info->rx_ctrl->rssi : -127;
  instance_->onReceive(src, data, len, rssi);
#else
  instance_->onReceive(mac, data, len, -127);
#endif
}

} // namespace ghost
