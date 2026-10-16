#include "ghost_ble.h"
#include "ghost_crypto.h"
#include "ghost_pair.h"
#include <NimBLEDevice.h>
#include <esp_wifi.h>
namespace ghost {
GhostBle Ble;
static GhostBle* instance = nullptr;
struct __attribute__((packed)) BleHint {
  uint16_t company; char magic[2]; uint8_t version, channel; uint16_t network; uint8_t tag[8];
};
static_assert(sizeof(BleHint) + 5 <= 31, "Legacy BLE advertisement");
bool GhostBle::signHint(const uint8_t* bytes, size_t length, uint8_t out[8]) {
  uint8_t key[32]{}, digest[32]{};
  if (!deriveKey(ghostpair::HOP_KEY, 32, "GHOSTLINK BLE v4", key) || !hmac256(key, 32, bytes, length, digest)) return false;
  memcpy(out, digest, 8); return true;
}
class GhostScanCallbacks final : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    if (!instance || !instance->hints_ || !dev->haveManufacturerData()) return;
    const auto data = dev->getManufacturerData();
    if (data.size() != sizeof(BleHint)) return;
    BleHint hint{}; memcpy(&hint, data.data(), sizeof(hint));
    if (hint.company != 0xffff || memcmp(hint.magic, "GL", 2) || hint.version != PROTOCOL_VERSION ||
        hint.network != ghostpair::NETWORK_ID || hint.channel < 1 || hint.channel > 11) return;
    uint8_t tag[8]{};
    if (!GhostBle::signHint(reinterpret_cast<const uint8_t*>(&hint), offsetof(BleHint, tag), tag) || !equalTag(tag, hint.tag, 8)) return;
    const GhostBle::HintEvent event{hint.channel, millis()};
    xQueueOverwrite(instance->hints_, &event);
    NimBLEDevice::getScan()->stop();
  }
};
static GhostScanCallbacks callbacks;
void GhostBle::begin(uint8_t) {
  if (!ENABLE_BLE_RESCUE || GHOST_PAIR_CONFIGURED != 1) return;
  // Check before entering the controller: its coexistence assertion is fatal.
  wifi_ps_type_t powerSave;
  if (esp_wifi_get_ps(&powerSave) != ESP_OK || powerSave == WIFI_PS_NONE) {
    Serial.println("[ble] unavailable: Wi-Fi modem sleep must be enabled for coexistence");
    return;
  }
  hints_ = xQueueCreate(1, sizeof(HintEvent));
  if (!hints_) { Serial.println("[ble] hint queue allocation failed"); return; }
  instance = this;
  if (!NimBLEDevice::init("")) {
    Serial.println("[ble] controller initialization failed; continuing with ESP-NOW");
    instance = nullptr; vQueueDelete(hints_); hints_ = nullptr;
    return;
  }
  auto* adv = NimBLEDevice::getAdvertising();
  adv->setConnectableMode(BLE_GAP_CONN_MODE_NON); adv->enableScanResponse(false);
  adv->setMinInterval(320); adv->setMaxInterval(480);
  auto* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&callbacks, false); scan->setActiveScan(false);
  scan->setInterval(101); scan->setWindow(31); scan->setMaxResults(0);
  started_ = true;
  Serial.println("[ble] rendezvous ready");
}
bool GhostBle::updateAdvertisement(uint8_t channel) {
  BleHint hint{}; hint.company = 0xffff; memcpy(hint.magic, "GL", 2);
  hint.version = PROTOCOL_VERSION; hint.channel = channel; hint.network = ghostpair::NETWORK_ID;
  if (!signHint(reinterpret_cast<const uint8_t*>(&hint), offsetof(BleHint, tag), hint.tag)) return false;
  NimBLEAdvertisementData data; data.setFlags(0x06);
  data.setManufacturerData(reinterpret_cast<const uint8_t*>(&hint), sizeof(hint));
  auto* adv = NimBLEDevice::getAdvertising(); adv->setAdvertisementData(data);
  return adv->isAdvertising() ? adv->refreshAdvertisingData() : adv->start();
}
void GhostBle::loop(uint8_t role, uint8_t channel, bool synced) {
  if (!started_) return;
  const auto now = millis();
  // Only synchronized nodes advertise; an old/replayed hint is never link proof.
  auto* adv = NimBLEDevice::getAdvertising();
  if (synced && now - lastAdv_ >= BLE_ADV_REFRESH_MS) { updateAdvertisement(channel); lastAdv_ = now; }
  if (!synced && adv->isAdvertising()) adv->stop();
  auto* scan = NimBLEDevice::getScan();
  if (synced || role == 0) { if (scan->isScanning()) scan->stop(); xQueueReset(hints_); return; }
  if (now - lastScan_ >= BLE_SCAN_GAP_MS && !scan->isScanning()) { lastScan_ = now; scan->start(BLE_SCAN_MS, false, true); }
}
bool GhostBle::consumeChannel(uint8_t& channel) {
  HintEvent event{};
  if (!hints_ || xQueueReceive(hints_, &event, 0) != pdTRUE || millis() - event.at > 250) return false;
  channel = event.channel; return true;
}
} // namespace ghost
