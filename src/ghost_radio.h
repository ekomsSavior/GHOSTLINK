#pragma once
#include <Arduino.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "ghost_crypto.h"
#include "ghost_mesh.h"
namespace ghost {
struct RxEvent { uint8_t bytes[MAX_FRAME]{}; uint16_t length = 0; Mac source{}; uint32_t at = 0; int8_t rssi = -127; };
class GhostRadio {
 public:
  bool begin(); void loop();
  bool configured() const { return configured_; }
  bool ready() const { return ready_; }
  bool linked() const { return peerSeen_ && millis() - peerAt_ < 30000; }
  bool synced() const { return configured_ && ready_ && sync_.synchronized; }
  bool hunting() const { return !synced(); }
  uint8_t role() const { return role_; }
  uint8_t channel() const { return channel_; }
  bool lrEnabled() const { return ENABLE_WIFI_LR; }
  int8_t rssi() const { return rssi_; }
  size_t neighbors() const { return peers_.count(millis()); }
  size_t queuedMessages() const { return queue_.queued(); }
  size_t relayMessages() const { return queue_.relayed(); }
  bool sendMessage(const String& text);
  bool popReceivedMessage(String& text, uint32_t& seq);
  bool restoredMessage(size_t index, String& text, uint32_t& seq) const;
  bool consumeDeliveryNotice(uint32_t& seq, bool& success);
  uint32_t lastQueuedSeq() const { return lastQueuedSeq_; }
  String localMacString() const;
  void printStatus() const;
  void printNeighbors() const;
  void applyCandidate(uint8_t channel);
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
  static void sniffThunk(void* buffer, wifi_promiscuous_pkt_type_t type);
  void onReceive(const uint8_t* mac, const uint8_t* data, int len, int8_t rssi);
  void process(const RxEvent& event);
  bool sendFrame(FrameType type, const uint8_t* data, size_t size, uint8_t hops = 0);
  bool sendEnvelope(const Envelope& envelope, uint8_t hops);
  bool setChannel(uint8_t channel);
  void updateSchedule(uint32_t now);
  void updateActivity(uint32_t now);
  void reviewChannels(uint32_t now);
  bool save(); bool load();
  bool endpoints(const Envelope& e) const;
  void queueAck(const Envelope& data);
  const uint8_t role_ = GHOST_NODE_ROLE;
  Mac local_{}, a_{}, b_{};
  bool configured_ = false, ready_ = false, busy_ = false, peerSeen_ = false;
  uint8_t channel_ = 0, home_ = 1, candidateChannel_ = 0;
  int8_t rssi_ = -127, txPower_ = 0;
  uint32_t boot_ = 0, frame_ = 0, messageSeq_ = 0, lastQueuedSeq_ = 0, seed_ = 0;
  uint64_t session_ = 0;
  uint32_t peerAt_ = 0;
  uint32_t nextHello_ = 0, nextTx_ = 0, busyAt_ = 0, candidateUntil_ = 0;
  uint32_t activityAt_ = 0, reviewAt_ = 0, expiryAt_ = 0;
  uint32_t outDue_[OUTBOX_DEPTH]{}, relayDue_[RELAY_DEPTH]{}, relayBorn_[RELAY_DEPTH]{};
  uint32_t dwell_[11]{}, counts_[11]{};
  uint8_t activity_[11]{};
  uint8_t relayAttempts_[RELAY_DEPTH]{};
  size_t sendCursor_ = 0;
  MeshClock sync_;
  MeshCrypto crypto_; NeighborTable peers_; MeshQueue queue_;
  MeshState backup_{};
  Preferences storage_;
  StoredEnvelope ackQueue_[MESSAGE_QUEUE_DEPTH]{};
  QueueHandle_t rxQueue_ = nullptr, txQueue_ = nullptr, deliveryQueue_ = nullptr;
  portMUX_TYPE monitorMux_ = portMUX_INITIALIZER_UNLOCKED;
  uint32_t sniffCounts_[11]{};
  uint32_t txCount_ = 0, rxCount_ = 0, rejected_ = 0, duplicates_ = 0, txFailed_ = 0, storeFailed_ = 0;
};
extern GhostRadio Radio;
} // namespace ghost
