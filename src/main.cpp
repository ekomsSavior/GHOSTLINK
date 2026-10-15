#include <Arduino.h>
#include "ghost_radio.h"
#include "ghost_ble.h"
#include "ghost_touch.h"
#include "ghost_ui.h"
#include "ghost_config.h"

using namespace ghost;

void setup() {
  Serial.begin(115200);
  delay(250);
  Serial.printf("\nGHOSTLINK v%s / node %c\n", VERSION, GHOST_NODE_ROLE == 0 ? 'A' : 'B');

  if (!Radio.begin()) {
    Serial.println("[fatal] radio initialization failed");
  }
  if (!Touch.begin()) {
    Serial.println("[warn] touchscreen initialization failed");
  }

  UI.begin();
  Ble.begin(Radio.role());
}

void loop() {
  Radio.loop();

  Ble.loop(Radio.role(), Radio.slot(), Radio.msIntoSlot(),
           Radio.synced(), Radio.ageSincePeerMs());

  uint32_t bleSlot = 0;
  uint16_t bleMs = 0;
  if (Ble.consumeSync(bleSlot, bleMs)) {
    Radio.applyExternalSync(bleSlot, bleMs);
  }

  UI.loop();
  delay(4);
}
