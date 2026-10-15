#pragma once
#include <cstddef>
#include <cstdint>

namespace ghost {

constexpr char VERSION[] = "0.4.0";
constexpr uint8_t PROTOCOL_VERSION = 4;

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

// The ESP32 transmits its own LR frames. Ordinary Wi-Fi reception is enabled
// so ambient packet activity can inform channel selection.
constexpr bool ENABLE_WIFI_LR = true;
constexpr bool ENABLE_CHANNEL_ADAPTATION = true;
constexpr int8_t WIFI_MAX_TX_POWER_QDBM = 80;
constexpr uint32_t HOP_INTERVAL_MS = 3000;
constexpr uint32_t HOP_GUARD_MS = 150;
constexpr uint32_t RENDEZVOUS_EVERY_SLOTS = 8;
constexpr uint32_t HELLO_INTERVAL_MS = 1100;
constexpr uint32_t PEER_LOST_MS = 10000;
constexpr uint32_t TX_SPACING_MS = 55;
constexpr uint32_t ACK_TIMEOUT_MS = 3500;
constexpr uint32_t RELAY_RETRY_MS = 4500;
constexpr uint32_t RELAY_RETENTION_MS = 20 * 60 * 1000;
constexpr uint32_t ACK_RETENTION_MS = RELAY_RETENTION_MS;
constexpr uint32_t CHANNEL_REVIEW_MS = 60000;
constexpr uint16_t ALL_CHANNELS = 0x07ff; // Channels 1..11.
constexpr uint8_t MAX_HOPS = 8;
constexpr size_t MAX_NEIGHBORS = 16;
constexpr size_t RELAY_DEPTH = 12;
constexpr size_t MAX_TEXT = 160;
constexpr size_t CHAT_LINES = 24;
constexpr size_t RX_QUEUE_DEPTH = 12;
constexpr size_t OUTBOX_DEPTH = 8;
constexpr size_t MESSAGE_QUEUE_DEPTH = 8;
constexpr size_t MESSAGE_DEDUP_DEPTH = 32;
constexpr bool ENABLE_BLE_RESCUE = true;
constexpr uint32_t BLE_ADV_REFRESH_MS = 700;
constexpr uint32_t BLE_SCAN_MS = 600;
constexpr uint32_t BLE_SCAN_GAP_MS = 2400;
constexpr uint32_t BLE_CANDIDATE_MS = 1800;
} // namespace ghost
