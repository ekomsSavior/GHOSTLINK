#include "ghost_touch.h"

namespace ghost {

GhostTouch Touch;

bool GhostTouch::begin() {
  spi_.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  if (!ts_.begin(spi_)) return false;
  ts_.setRotation(0); // Keep raw panel orientation; map explicitly below.
  return true;
}

bool GhostTouch::read(TouchPoint& out) {
  if (!ts_.touched()) { pressed_ = false; return false; }
  if (pressed_) return false;
  TS_Point p = ts_.getPoint();
  if (p.z < 80) return false;

  // Common CYD landscape transform: panel raw-Y -> screen-X, raw-X ->
  // inverted screen-Y. If your panel differs, adjust the four calibration
  // values in ghost_config.h or flip these maps.
  int x = map(p.y, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, SCREEN_W - 1);
  int y = map(p.x, TOUCH_X_MIN, TOUCH_X_MAX, SCREEN_H - 1, 0);
  x = constrain(x, 0, SCREEN_W - 1);
  y = constrain(y, 0, SCREEN_H - 1);

  const uint32_t now = millis();
  if (now - lastTouchMs_ < 55) return false;
  lastTouchMs_ = now;
  pressed_ = true;

  out = {int16_t(x), int16_t(y), int16_t(p.z)};
  return true;
}

} // namespace ghost
