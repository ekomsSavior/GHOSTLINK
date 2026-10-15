#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include "ghost_config.h"

namespace ghost {

struct TouchPoint {
  int16_t x;
  int16_t y;
  int16_t z;
};

class GhostTouch {
 public:
  bool begin();
  bool read(TouchPoint& p);

 private:
  SPIClass spi_{VSPI};
  XPT2046_Touchscreen ts_{TOUCH_CS, TOUCH_IRQ};
  uint32_t lastTouchMs_ = 0;
};

extern GhostTouch Touch;

} // namespace ghost
