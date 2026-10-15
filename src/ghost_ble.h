#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "ghost_config.h"

namespace ghost {
class GhostScanCallbacks;
class GhostBle {
 public:
  void begin(uint8_t role);
  void loop(uint8_t role, uint32_t slot, uint16_t phase, bool wifiSynced, uint32_t lostForMs);
  bool consumeSync(uint32_t& slot, uint16_t& phase);
 private:
  friend class GhostScanCallbacks;
  struct HintEvent { uint32_t slot; uint16_t phase; uint32_t receivedAt; };
  void updateAdvertisement(uint32_t slot, uint16_t phase);
  static void signHint(const uint8_t* bytes, size_t length, uint8_t out[8]);
  uint8_t role_ = 0;
  bool started_ = false;
  uint32_t lastAdvRefreshMs_ = 0, lastScanStartMs_ = 0;
  QueueHandle_t hintQueue_ = nullptr;
};
extern GhostBle Ble;
} // namespace ghost
