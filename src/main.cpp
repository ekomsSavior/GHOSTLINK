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
  if (Radio.ready() && Radio.configured()) Ble.begin(Radio.role());
  Serial.println("Type a message then Enter, /status for radio diagnostics, /help for help.");
}

static void serialConsole() {
  static String line;
  static bool overflow = false;
  for (unsigned n = 0; n < 64 && Serial.available(); ++n) {
    const char ch = char(Serial.read());
    if (ch == '\r') continue;
    if (ch == '\n') {
      line.trim();
      if (overflow) Serial.println("[console] message too long; limit 160 bytes");
      else if (line == "/status") Radio.printStatus();
      else if (line == "/help") Serial.println("Plain text: queue message. /status: link/outbox/counters. Messages stay queued while powered on.");
      else if (line.length()) UI.queueOutgoing(line);
      line = ""; overflow = false;
    } else if (ch == 8 || ch == 127) {
      if (line.length()) line.remove(line.length() - 1);
    } else if (uint8_t(ch) >= 32) {
      if (line.length() < MAX_TEXT) line += ch;
      else overflow = true;
    }
  }
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

  serialConsole();
  UI.loop();
  delay(4);
}
