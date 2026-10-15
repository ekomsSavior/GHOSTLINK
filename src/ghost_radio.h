#pragma once

#include <Arduino.h>
#include <esp_now.h>
#include <esp_idf_version.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "ghost_config.h"

namespace ghost {

enum class PacketType : uint8_t {
  SYNC = 1,
  MESSAGE = 2,
  ACK = 3,
  PING = 4,
};

struct __attribute__((packed)) GhostPacket {
  char magic[4];            // "GHST"
  uint8_t version;
  uint8_t type;
  uint8_t senderRole;
  uint8_t flags;
  uint32_t seq;
  uint32_t ack;
  uint32_t slot;
  uint16_t msIntoSlot;
  uint16_t payloadLen;
  char payload[MAX_TEXT + 1];
};

struct RxEvent {
  GhostPacket packet;
  int8_t rssi;
};

class GhostRadio {
 public:
  bool begin();
  void loop();

  bool configured() const { return configured_; }
  bool linked() const;
  bool synced() const { return role_ == 0 || synced_; }
  bool hunting() const { return role_ == 1 && !synced_; }
  uint8_t channel() const { return currentChannel_; }
  uint32_t slot() const { return currentSlot_; }
  uint16_t msIntoSlot() const;
  int8_t rssi() const { return lastRssi_; }
  bool lrEnabled() const { return lrEnabled_; }
  int8_t txPowerQdbm() const { return txPowerQdbm_; }
  uint32_t ageSincePeerMs() const;
  uint8_t role() const { return role_; }

  bool sendMessage(const String& text);
  bool popReceivedMessage(String& outText, uint32_t& seq);

  bool hasPending() const { return pendingActive_; }
  uint32_t pendingSeq() const { return pending_.seq; }
  bool consumeDeliveryNotice(uint32_t& seq, bool& success);

  String localMacString() const;
  String peerMacString() const;

  // Called by BLE rendezvous after validating a signed hint.
  void applyExternalSync(uint32_t slot, uint16_t msIntoSlot);

 private:
  static GhostRadio* instance_;
  static void recvThunk(
#if ESP_IDF_VERSION_MAJOR >= 5
      const esp_now_recv_info_t* info,
#else
      const uint8_t* mac,
#endif
      const uint8_t* data, int len);

  void onReceive(const uint8_t* src, const uint8_t* data, int len, int8_t rssi);
  void drainRx();
  void processPacket(const RxEvent& ev);
  void setChannel(uint8_t ch);
  void configurePhy();
  uint8_t channelForSlot(uint32_t slot) const;
  void leaderLoop(uint32_t now);
  void followerLoop(uint32_t now);
  void enterHunt();
  void hopFollower(uint32_t now);
  void sendSync(uint32_t now);
  void sendPing();
  bool sendPacket(GhostPacket& p);
  GhostPacket makePacket(PacketType type);
  void sendAck(uint32_t seq);
  bool srcIsPeer(const uint8_t* src) const;
  const uint8_t* peerMac() const;

  bool configured_ = false;
  uint8_t role_ = GHOST_NODE_ROLE;
  uint8_t currentChannel_ = 0;
  uint32_t currentSlot_ = 1;
  uint32_t slotStartMs_ = 0;
  uint32_t lastHopMs_ = 0;
  uint32_t lastSyncTxMs_ = 0;
  uint32_t lastPingMs_ = 0;
  uint32_t lastPeerRxMs_ = 0;
  int8_t lastRssi_ = -127;
  bool synced_ = false;
  uint8_t huntChannel_ = 1;
  uint32_t huntSinceMs_ = 0;
  uint32_t huntStartedMs_ = 0;
  bool lrEnabled_ = false;
  int8_t txPowerQdbm_ = 0;

  uint32_t txSeq_ = 1;
  uint32_t lastRxMessageSeq_ = 0;

  GhostPacket pending_{};
  bool pendingActive_ = false;
  uint8_t pendingRetries_ = 0;
  uint32_t pendingLastTxMs_ = 0;

  bool deliveryNotice_ = false;
  uint32_t deliverySeq_ = 0;
  bool deliverySuccess_ = false;

  QueueHandle_t rxQueue_ = nullptr;
  QueueHandle_t messageQueue_ = nullptr;
};

extern GhostRadio Radio;

} // namespace ghost
