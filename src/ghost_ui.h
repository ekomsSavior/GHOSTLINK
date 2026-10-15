#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>
#include "ghost_config.h"

namespace ghost {

struct ChatLine {
  bool mine = false;
  String text;
  uint32_t seq = 0;
  char state = ' '; // '.' pending, '+' delivered, '!' failed
};

class GhostUI {
 public:
  struct Key { int16_t x, y, w, h; const char* label; char value; };

  void begin();
  void loop();
  void addIncoming(const String& text, uint32_t seq);
  void markDelivery(uint32_t seq, bool ok);
  bool queueOutgoing(const String& text);

 private:
  void drawAll();
  void drawHeader();
  void drawChat();
  void drawComposer();
  void drawKeyboard();
  void handleTouch(int x, int y);
  void pushLine(bool mine, const String& text, uint32_t seq, char state);
  bool sendComposer();
  int findKey(int x, int y) const;

  TFT_eSPI tft_;
  ChatLine lines_[CHAT_LINES];
  size_t lineCount_ = 0;
  size_t historyOffset_ = 0;
  String composer_;
  uint32_t lastHeaderMs_ = 0;
  bool dirtyChat_ = true;
  bool dirtyComposer_ = true;
  bool symbols_ = false;
};

extern GhostUI UI;

} // namespace ghost
