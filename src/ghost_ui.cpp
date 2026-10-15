#include "ghost_ui.h"

#include "ghost_radio.h"
#include "ghost_touch.h"

namespace ghost {

GhostUI UI;

static constexpr uint16_t C_BG = TFT_BLACK;
static constexpr uint16_t C_FG = TFT_WHITE;
static constexpr uint16_t C_DIM = 0x7BEF;
static constexpr uint16_t C_LINE = 0x4208;

static const GhostUI::Key KEYS[] = {
  {  2, 150, 30, 20, "Q", 'q'}, { 34,150,30,20,"W",'w'}, {66,150,30,20,"E",'e'}, {98,150,30,20,"R",'r'}, {130,150,30,20,"T",'t'}, {162,150,30,20,"Y",'y'}, {194,150,30,20,"U",'u'}, {226,150,30,20,"I",'i'}, {258,150,30,20,"O",'o'}, {290,150,28,20,"P",'p'},
  { 16, 173, 30, 20, "A", 'a'}, {48,173,30,20,"S",'s'}, {80,173,30,20,"D",'d'}, {112,173,30,20,"F",'f'}, {144,173,30,20,"G",'g'}, {176,173,30,20,"H",'h'}, {208,173,30,20,"J",'j'}, {240,173,30,20,"K",'k'}, {272,173,30,20,"L",'l'},
  { 35, 196, 30, 20, "Z", 'z'}, {67,196,30,20,"X",'x'}, {99,196,30,20,"C",'c'}, {131,196,30,20,"V",'v'}, {163,196,30,20,"B",'b'}, {195,196,30,20,"N",'n'}, {227,196,30,20,"M",'m'},
  {  2, 219, 55, 19, "123", 0}, {60,219,135,19,"SPACE",' '}, {198,219,55,19,"<-",8}, {256,219,62,19,"SEND",13}
};
static constexpr int KEY_COUNT = sizeof(KEYS) / sizeof(KEYS[0]);
static constexpr char SYMBOLS[] = "1234567890!@#$%&*().,?:/-+";
static_assert(sizeof(SYMBOLS) - 1 == 26, "One symbol per letter key");

void GhostUI::begin() {
  pinMode(TFT_BACKLIGHT, OUTPUT);
  digitalWrite(TFT_BACKLIGHT, HIGH);

  tft_.init();
  tft_.setRotation(1);
  tft_.setTextWrap(false);
  tft_.fillScreen(C_BG);
  drawAll();
}

void GhostUI::drawAll() {
  tft_.fillScreen(C_BG);
  drawHeader();
  drawChat();
  drawComposer();
  drawKeyboard();
}

void GhostUI::drawHeader() {
  tft_.fillRect(0, 0, 320, 24, C_BG);
  tft_.drawFastHLine(0, 23, 320, C_LINE);

  tft_.setTextColor(C_FG, C_BG);
  tft_.setTextFont(2);
  tft_.setCursor(4, 4);
  tft_.print("GHOSTLINK");

  tft_.setTextColor(C_DIM, C_BG);
  tft_.setCursor(110, 4);
  if (!Radio.ready()) {
    tft_.print("ERROR");
  } else if (!Radio.configured()) {
    tft_.print("SETUP");
  } else if (Radio.hunting()) {
    tft_.print("HUNT");
  } else if (Radio.linked()) {
    tft_.print("LINK");
  } else {
    tft_.print("SYNC");
  }

  tft_.setCursor(174, 4);
  tft_.printf("CH%02u", Radio.channel());

  tft_.setCursor(210, 4);
  tft_.print(Radio.lrEnabled() ? "LR" : "N");

  tft_.setCursor(238, 4);
  if (Radio.queuedMessages()) tft_.printf("Q:%u", unsigned(Radio.queuedMessages()));
  else if (Radio.rssi() > -126) tft_.printf("%ddBm", Radio.rssi());
  else tft_.print("--dBm");
}

void GhostUI::drawChat() {
  tft_.fillRect(0, 25, 320, 94, C_BG);
  int y = 28;

  if (!Radio.ready() || !Radio.configured()) {
    tft_.setTextColor(C_FG, C_BG);
    tft_.setTextFont(2);
    tft_.setCursor(6, 31);
    tft_.print(Radio.ready() ? "PAIR CONFIG REQUIRED" : "RADIO ERROR - CHECK SERIAL");
    tft_.setTextColor(C_DIM, C_BG);
    tft_.setTextFont(1);
    tft_.setCursor(6, 52);
    tft_.print("LOCAL MAC:");
    tft_.setCursor(6, 65);
    tft_.print(Radio.localMacString());
    tft_.setCursor(6, 84);
    tft_.print("Run tools/make_pair.py with both MACs");
    return;
  }

  const size_t end = lineCount_ - historyOffset_;
  const size_t start = end > 2 ? end - 2 : 0;
  tft_.setTextFont(1);
  for (size_t i = start; i < end; ++i) {
    const ChatLine& l = lines_[i];
    tft_.setTextColor(l.mine ? C_DIM : C_FG, C_BG);
    for (size_t offset = 0; offset < l.text.length(); offset += 47) {
      tft_.setCursor(5, y);
      tft_.print(offset == 0 ? (l.mine ? "> " : "< ") : "  ");
      tft_.print(l.text.substring(offset, offset + 47));
      if (offset == 0 && l.mine) {
        tft_.setCursor(304, y);
        tft_.print(l.state);
      }
      y += 9;
    }
    y += 4;
  }
  tft_.setTextColor(C_DIM, C_BG);
  tft_.setCursor(5, 111);
  tft_.print("< older                  newer >");
}

void GhostUI::drawComposer() {
  tft_.fillRect(0, 120, 320, 27, C_BG);
  tft_.drawRect(2, 121, 316, 24, C_LINE);
  tft_.setTextColor(C_FG, C_BG);
  tft_.setTextFont(1);
  tft_.setCursor(6, 129);
  String shown = composer_;
  if (shown.length() > 48) shown = shown.substring(shown.length() - 48);
  tft_.print(shown);
  tft_.print("_");
}

void GhostUI::drawKeyboard() {
  tft_.fillRect(0, 148, 320, 92, C_BG);
  for (int i = 0; i < KEY_COUNT; ++i) {
    const Key& k = KEYS[i];
    char symbol[2] = {i < 26 ? SYMBOLS[i] : char(0), 0};
    const char* label = symbols_ && i < 26 ? symbol : (i == 26 && symbols_ ? "ABC" : k.label);
    tft_.drawRect(k.x, k.y, k.w, k.h, C_LINE);
    tft_.setTextColor(C_FG, C_BG);
    tft_.setTextFont(1);
    const int16_t tx = k.x + max<int16_t>(2, (k.w - int(strlen(label)) * 6) / 2);
    const int16_t ty = k.y + 6;
    tft_.setCursor(tx, ty);
    tft_.print(label);
  }
}

int GhostUI::findKey(int x, int y) const {
  for (int i = 0; i < KEY_COUNT; ++i) {
    const Key& k = KEYS[i];
    if (x >= k.x && x < k.x + k.w && y >= k.y && y < k.y + k.h) return i;
  }
  return -1;
}

void GhostUI::handleTouch(int x, int y) {
  if (y >= 25 && y < 120 && lineCount_) {
    if (x < SCREEN_W / 2) historyOffset_ = min<size_t>(historyOffset_ + 2, lineCount_ - 1);
    else historyOffset_ = historyOffset_ > 2 ? historyOffset_ - 2 : 0;
    dirtyChat_ = true;
    return;
  }
  const int idx = findKey(x, y);
  if (idx < 0) return;
  const char v = symbols_ && idx < 26 ? SYMBOLS[idx] : KEYS[idx].value;

  if (v == 13) {
    sendComposer();
  } else if (v == 8) {
    if (composer_.length()) composer_.remove(composer_.length() - 1);
  } else if (v == 0) {
    symbols_ = !symbols_;
    drawKeyboard();
  } else if (composer_.length() < MAX_TEXT) {
    composer_ += v;
  }
  dirtyComposer_ = true;
}

bool GhostUI::sendComposer() {
  composer_.trim();
  if (!composer_.length()) return false;
  if (!queueOutgoing(composer_)) return false;
  composer_ = "";
  dirtyChat_ = true;
  dirtyComposer_ = true;
  return true;
}

bool GhostUI::queueOutgoing(const String& text) {
  if (!Radio.sendMessage(text)) {
    Serial.println("[outbox] not queued: check pair/radio state, length, or full outbox; composer retained");
    return false;
  }
  const auto seq = Radio.lastQueuedSeq();
  pushLine(true, text, seq, '.');
  dirtyChat_ = true;
  Serial.printf("[queued #%lu] %s\n", (unsigned long)seq, text.c_str());
  return true;
}

void GhostUI::pushLine(bool mine, const String& text, uint32_t seq, char state) {
  historyOffset_ = 0;
  if (lineCount_ < CHAT_LINES) {
    lines_[lineCount_++] = {mine, text, seq, state};
  } else {
    for (size_t i = 1; i < CHAT_LINES; ++i) lines_[i - 1] = lines_[i];
    lines_[CHAT_LINES - 1] = {mine, text, seq, state};
  }
}

void GhostUI::addIncoming(const String& text, uint32_t seq) {
  Serial.printf("[received #%lu] %s\n", (unsigned long)seq, text.c_str());
  pushLine(false, text, seq, ' ');
  dirtyChat_ = true;
}

void GhostUI::markDelivery(uint32_t seq, bool ok) {
  Serial.printf("[delivered #%lu] %s\n", (unsigned long)seq, ok ? "peer accepted" : "failed");
  for (size_t i = 0; i < lineCount_; ++i) {
    if (lines_[i].mine && lines_[i].seq == seq) {
      lines_[i].state = ok ? '+' : '!';
      dirtyChat_ = true;
      return;
    }
  }
}

void GhostUI::loop() {
  TouchPoint p{};
  if (Touch.read(p)) handleTouch(p.x, p.y);

  String text;
  uint32_t seq = 0;
  while (Radio.popReceivedMessage(text, seq)) addIncoming(text, seq);

  bool ok = false;
  while (Radio.consumeDeliveryNotice(seq, ok)) markDelivery(seq, ok);

  const uint32_t now = millis();
  if (now - lastHeaderMs_ > 350) {
    drawHeader();
    lastHeaderMs_ = now;
  }
  if (dirtyChat_) {
    drawChat();
    dirtyChat_ = false;
  }
  if (dirtyComposer_) {
    drawComposer();
    dirtyComposer_ = false;
  }
}

} // namespace ghost
