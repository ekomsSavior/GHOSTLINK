#pragma once
#include <cstddef>
#include <cstdint>

namespace ghost {

constexpr char VERSION[] = "0.3.0";
constexpr uint8_t PROTOCOL_VERSION = 3;

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
constexpr uint32_t SYNC_INTERVAL_MS = 180;
constexpr uint32_t PEER_LOST_MS = 4600;
constexpr uint32_t HUNT_DWELL_MS = 300;
constexpr uint32_t ACK_TIMEOUT_MS = 1900;
constexpr uint8_t  MAX_RETRIES = 6;
constexpr uint32_t PING_INTERVAL_MS = 2200;
constexpr uint32_t RETRY_REST_MS = 5000;
constexpr uint32_t HOP_GUARD_MS = 60;
constexpr uint32_t TX_SPACING_MS = 16;
constexpr uint32_t RENDEZVOUS_EVERY_SLOTS = 8;
constexpr uint32_t BLE_CANDIDATE_MS = 700;

// BLE rendezvous is the rescue/control plane. It does NOT carry chat text and
// it does not extend the Wi-Fi RF range. Its job is to help a lost follower
// rediscover the anchor's current hopping epoch/channel quickly.
constexpr bool ENABLE_BLE_RESCUE = true;
constexpr uint32_t BLE_ADV_REFRESH_MS = 180;
constexpr uint32_t BLE_RESCUE_AFTER_MS = 1800;
constexpr uint32_t BLE_SCAN_MS = 900;
constexpr uint32_t BLE_SCAN_GAP_MS = 1200;

// After a long loss, park on the pair's periodic Wi-Fi rendezvous channel.
constexpr uint32_t DEEP_HUNT_AFTER_MS = 9000;

// UI / protocol limits.
constexpr size_t MAX_TEXT = 160;
constexpr size_t CHAT_LINES = 24;
constexpr size_t RX_QUEUE_DEPTH = 8;
constexpr size_t OUTBOX_DEPTH = 8;
constexpr size_t MESSAGE_QUEUE_DEPTH = 8;
constexpr size_t MESSAGE_DEDUP_DEPTH = 32;

} // namespace ghost
