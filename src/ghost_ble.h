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
  void loop(uint8_t role, uint8_t channel, bool synced);
  bool consumeChannel(uint8_t& channel);
 private:
  friend class GhostScanCallbacks;
  struct HintEvent { uint8_t channel; uint32_t at; };
  bool updateAdvertisement(uint8_t channel);
  static bool signHint(const uint8_t* bytes, size_t length, uint8_t out[8]);
  bool started_ = false;
  uint32_t lastAdv_ = 0, lastScan_ = 0;
  QueueHandle_t hints_ = nullptr;
};
extern GhostBle Ble;
} // namespace ghost
