#pragma once

#include <Arduino.h>
#include "ghost_config.h"

namespace ghost {

class GhostScanCallbacks;

class GhostBle {
 public:
  void begin(uint8_t role);
  void loop(uint8_t role, uint32_t slot, uint16_t msIntoSlot, bool wifiSynced, uint32_t lostForMs);
  bool consumeSync(uint32_t& slot, uint16_t& msIntoSlot);

 private:
  friend class GhostScanCallbacks;
  void updateAdvertisement(uint32_t slot, uint16_t msIntoSlot);
  void maybeStartScan(uint32_t lostForMs);
  static void signHint(uint32_t slot, uint16_t msIntoSlot, uint8_t out[4]);
  static bool verifyHint(uint32_t slot, uint16_t msIntoSlot, const uint8_t tag[4]);

  uint8_t role_ = 0;
  bool started_ = false;
  uint32_t lastAdvRefreshMs_ = 0;
  uint32_t lastScanStartMs_ = 0;
  volatile bool syncReady_ = false;
  volatile uint32_t syncSlot_ = 0;
  volatile uint16_t syncMs_ = 0;
};

extern GhostBle Ble;

} // namespace ghost
