#pragma once
#include <Arduino.h>

namespace ghost {

constexpr char VERSION[] = "0.2.0";
constexpr uint8_t PROTOCOL_VERSION = 2;

// CYD / ESP32-2432S028R touchscreen pins.
constexpr int TOUCH_IRQ  = 36;
constexpr int TOUCH_MOSI = 32;
constexpr int TOUCH_MISO = 39;
constexpr int TOUCH_CLK  = 25;
constexpr int TOUCH_CS   = 33;

constexpr int TFT_BACKLIGHT = 21;

// Display is landscape after tft.setRotation(1).
constexpr int SCREEN_W = 320;
constexpr int SCREEN_H = 240;

// Starting calibration for many 2.8" CYDs. Every resistive panel is a little
// different; tune these four numbers if the keyboard does not line up.
constexpr int TOUCH_X_MIN = 280;
constexpr int TOUCH_X_MAX = 3860;
constexpr int TOUCH_Y_MIN = 340;
constexpr int TOUCH_Y_MAX = 3860;

// ---------------------------------------------------------------------------
// Radio profile
// ---------------------------------------------------------------------------
// GHOST-LR uses Espressif's proprietary 802.11 LR PHY between the two ESP32s.
// No access point is involved. Both nodes must use the same profile.
constexpr bool ENABLE_WIFI_LR = true;
constexpr bool LR_ONLY_PHY = true;   // true = LR only, false = B/G/N + LR

// 80 quarter-dBm = 20 dBm. The ESP-IDF API clamps to chip/country limits.
// Do not raise this beyond legal/regulatory limits for your location.
constexpr int8_t WIFI_MAX_TX_POWER_QDBM = 80;

// LR is slower than normal Wi-Fi, so give each channel a little more dwell
// time than v0.1. This also improves edge-of-range message/ACK reliability.
constexpr uint32_t HOP_INTERVAL_MS = 1800;
constexpr uint32_t SYNC_INTERVAL_MS = 240;
constexpr uint32_t PEER_LOST_MS = 4600;
constexpr uint32_t HUNT_DWELL_MS = 145;
constexpr uint32_t ACK_TIMEOUT_MS = 1150;
constexpr uint8_t  MAX_RETRIES = 10;
constexpr uint32_t PING_INTERVAL_MS = 2200;

// BLE rendezvous is the rescue/control plane. It does NOT carry chat text and
// it does not extend the Wi-Fi RF range. Its job is to help a lost follower
// rediscover the anchor's current hopping epoch/channel quickly.
constexpr bool ENABLE_BLE_RESCUE = true;
constexpr uint32_t BLE_ADV_REFRESH_MS = 180;
constexpr uint32_t BLE_RESCUE_AFTER_MS = 1800;
constexpr uint32_t BLE_SCAN_MS = 900;
constexpr uint32_t BLE_SCAN_GAP_MS = 1200;

// When the follower has been lost for a long time, temporarily slow its Wi-Fi
// hunt so BLE coexistence has more airtime to hear the signed rendezvous hint.
constexpr uint32_t DEEP_HUNT_AFTER_MS = 9000;
constexpr uint32_t DEEP_HUNT_DWELL_MS = 260;

// UI / protocol limits.
constexpr size_t MAX_TEXT = 160;
constexpr size_t CHAT_LINES = 7;
constexpr size_t RX_QUEUE_DEPTH = 8;

} // namespace ghost
