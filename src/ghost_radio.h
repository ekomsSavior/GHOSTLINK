#pragma once
#include <Arduino.h>
#include <esp_now.h>
#include <esp_idf_version.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "ghost_protocol.h"

namespace ghost {
struct RxEvent { GhostPacket packet; uint32_t receivedAt; int8_t rssi; };

class GhostRadio {
 public:
  bool begin();
  void loop();
  bool configured() const { return configured_; }
  bool ready() const { return ready_; }
  bool linked() const;
  bool synced() const { return configured_ && ready_ && (role_ == 0 || synced_); }
  bool hunting() const { return role_ == 1 && !synced_; }
  uint8_t channel() const { return currentChannel_; }
  uint32_t slot() const { return clock_.slot; }
  uint16_t msIntoSlot() const { return clock_.phase(millis()); }
  int8_t rssi() const { return lastRssi_; }
  bool lrEnabled() const { return lrEnabled_; }
  int8_t txPowerQdbm() const { return txPowerQdbm_; }
  uint32_t ageSincePeerMs() const { return millis() - lastPeerRxMs_; }
  uint8_t role() const { return role_; }
  size_t queuedMessages() const { return outbox_.size(); }
  bool sendMessage(const String& text);
  bool popReceivedMessage(String& text, uint32_t& seq);
  uint32_t lastQueuedSeq() const { return lastQueuedSeq_; }
  bool consumeDeliveryNotice(uint32_t& seq, bool& success);
  String localMacString() const;
  String peerMacString() const;
  void printStatus() const;
  void applyExternalSync(uint32_t slot, uint16_t phase);

 private:
  static GhostRadio* instance_;
  static void recvThunk(
#if ESP_IDF_VERSION_MAJOR >= 5
      const esp_now_recv_info_t* info,
#else
      const uint8_t* mac,
#endif
      const uint8_t* data, int len);
  static void sentThunk(const uint8_t*, esp_now_send_status_t status);
  void onReceive(const uint8_t* mac, const uint8_t* data, int len, int8_t rssi);
  void processPacket(const RxEvent& ev);
  bool setChannel(uint8_t ch);
  bool configurePhy();
  uint8_t channelForSlot(uint32_t slot) const;
  uint8_t rendezvousChannel() const;
  void enterHunt(uint32_t now);
  void updateChannel(uint32_t now);
  GhostPacket makePacket(PacketType type) const;
  bool sendPacket(GhostPacket& packet, bool urgent);
  void signPacket(GhostPacket& packet) const;
  bool verifyPacket(const GhostPacket& packet) const;
  const uint8_t* peerMac() const;
  const uint8_t role_ = GHOST_NODE_ROLE;
  bool configured_ = false, ready_ = false, synced_ = false, peerSeen_ = false;
  bool lrEnabled_ = false, txBusy_ = false, candidate_ = false;
  uint8_t currentChannel_ = 0, huntChannel_ = 1;
  int8_t lastRssi_ = -127, txPowerQdbm_ = 0;
  SlotClock clock_;
  uint32_t session_ = 0, frame_ = 0, messageSeq_ = 0, lastQueuedSeq_ = 0;
  uint32_t lastPeerRxMs_ = 0, lastSyncTxMs_ = 0, lastPingMs_ = 0, lastTxMs_ = 0;
  uint32_t huntStartedMs_ = 0, huntSinceMs_ = 0, candidateSinceMs_ = 0;
  uint8_t candidateChannel_ = 1;
  uint32_t retryStartedMs_ = 0, retryRestStartedMs_ = 0;
  uint8_t attempts_ = 0;
  bool retryRest_ = false;
  bool ackPending_ = false;
  uint32_t ackSeq_ = 0, ackSession_ = 0;
  Outbox outbox_;
  ReplayWindow replay_;
  MessageDedup dedup_;
  uint32_t txCount_ = 0, txFailed_ = 0, rxCount_ = 0, duplicateCount_ = 0, rejectedCount_ = 0;
  QueueHandle_t rxQueue_ = nullptr, messageQueue_ = nullptr, txQueue_ = nullptr, deliveryQueue_ = nullptr;
};
extern GhostRadio Radio;
} // namespace ghost
