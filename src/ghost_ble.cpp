#include "ghost_ble.h"
#include <NimBLEDevice.h>
#include <mbedtls/md.h>
#include <cstring>
#include "ghost_pair.h"

namespace ghost {
GhostBle Ble;
static GhostBle* gBle = nullptr;
struct __attribute__((packed)) BleHint {
  uint16_t company;
  char magic[2];
  uint8_t version, role;
  uint16_t networkId;
  uint32_t slot;
  uint16_t phase;
  uint8_t tag[8];
};
static_assert(sizeof(BleHint) + 2 + 3 <= 31, "Must fit legacy BLE advertisement");

void GhostBle::signHint(const uint8_t* bytes, size_t length, uint8_t out[8]) {
  const auto* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  uint8_t key[32]{}, digest[32]{};
  constexpr uint8_t domain[] = "GHOSTLINK BLE v3";
  mbedtls_md_hmac(md, ghostpair::HOP_KEY, 32, domain, sizeof(domain) - 1, key);
  mbedtls_md_hmac(md, key, sizeof(key), bytes, length, digest);
  memcpy(out, digest, 8);
}
class GhostScanCallbacks final : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    if (!gBle || !gBle->hintQueue_ || !dev->haveManufacturerData()) return;
    const auto raw = dev->getManufacturerData();
    if (raw.size() != sizeof(BleHint)) return;
    BleHint hint{}; memcpy(&hint, raw.data(), sizeof(hint));
    if (hint.company != 0xFFFF || memcmp(hint.magic, "GL", 2) ||
        hint.version != PROTOCOL_VERSION || hint.role != 0 ||
        hint.networkId != ghostpair::NETWORK_ID || hint.phase >= HOP_INTERVAL_MS) return;
    uint8_t expected[8]{}, diff = 0;
    GhostBle::signHint(reinterpret_cast<const uint8_t*>(&hint), offsetof(BleHint, tag), expected);
    for (size_t i = 0; i < sizeof(expected); ++i) diff |= expected[i] ^ hint.tag[i];
    if (diff) return;
    const GhostBle::HintEvent event{hint.slot, hint.phase, millis()};
    // FreeRTOS queue safely transfers one coherent hint between ESP32 cores.
    xQueueOverwrite(gBle->hintQueue_, &event);
    NimBLEDevice::getScan()->stop();
  }
};
static GhostScanCallbacks callbacks;

void GhostBle::begin(uint8_t role) {
  if (!ENABLE_BLE_RESCUE || GHOST_PAIR_CONFIGURED != 1) return;
  role_ = role;
  hintQueue_ = xQueueCreate(1, sizeof(HintEvent));
  if (!hintQueue_) { Serial.println("[BLE] no hint queue; using Wi-Fi recovery"); return; }
  gBle = this;
  NimBLEDevice::init("");
  if (role_ == 0) {
    auto* adv = NimBLEDevice::getAdvertising();
    adv->setConnectableMode(BLE_GAP_CONN_MODE_NON);
    adv->enableScanResponse(false);
    adv->setMinInterval(160); // 100 ms in 0.625 ms BLE advertising units.
    adv->setMaxInterval(240);
    updateAdvertisement(0, 0);
    if (!adv->start()) { Serial.println("[BLE] advertising failed"); return; }
  } else {
    auto* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&callbacks, false);
    scan->setActiveScan(false);
    scan->setInterval(97);
    scan->setWindow(47);
    scan->setMaxResults(0);
  }
  started_ = true;
}
void GhostBle::updateAdvertisement(uint32_t slot, uint16_t phase) {
  BleHint hint{};
  hint.company = 0xFFFF; memcpy(hint.magic, "GL", 2);
  hint.version = PROTOCOL_VERSION; hint.networkId = ghostpair::NETWORK_ID;
  hint.slot = slot; hint.phase = phase;
  signHint(reinterpret_cast<const uint8_t*>(&hint), offsetof(BleHint, tag), hint.tag);
  NimBLEAdvertisementData data;
  data.setFlags(0x06);
  data.setManufacturerData(reinterpret_cast<const uint8_t*>(&hint), sizeof(hint));
  auto* adv = NimBLEDevice::getAdvertising();
  adv->setAdvertisementData(data);
  if (adv->isAdvertising()) adv->refreshAdvertisingData();
}
void GhostBle::loop(uint8_t role, uint32_t slot, uint16_t phase, bool wifiSynced, uint32_t lostForMs) {
  if (!started_) return;
  const auto now = millis();
  if (role == 0) {
    if (now - lastAdvRefreshMs_ >= BLE_ADV_REFRESH_MS) {
      updateAdvertisement(slot, phase); lastAdvRefreshMs_ = now;
    }
    return;
  }
  auto* scan = NimBLEDevice::getScan();
  if (wifiSynced) {
    if (scan->isScanning()) scan->stop();
    xQueueReset(hintQueue_);
    return;
  }
  if (lostForMs >= BLE_RESCUE_AFTER_MS && now - lastScanStartMs_ >= BLE_SCAN_GAP_MS && !scan->isScanning()) {
    lastScanStartMs_ = now;
    scan->start(BLE_SCAN_MS, false, true);
  }
}
bool GhostBle::consumeSync(uint32_t& slot, uint16_t& phase) {
  HintEvent event{};
  if (!hintQueue_ || xQueueReceive(hintQueue_, &event, 0) != pdTRUE || millis() - event.receivedAt > 250) return false;
  slot = event.slot; phase = event.phase; return true;
}
} // namespace ghost
