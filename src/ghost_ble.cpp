#include "ghost_ble.h"

#include <NimBLEDevice.h>
#include <mbedtls/md.h>
#include <cstring>

#include "ghost_pair.h"

namespace ghost {

GhostBle Ble;
static GhostBle* gBle = nullptr;

struct __attribute__((packed)) BleHint {
  uint8_t companyLo;
  uint8_t companyHi;
  char magic[2];
  uint8_t version;
  uint8_t role;
  uint16_t networkId;
  uint32_t slot;
  uint16_t msIntoSlot;
  uint8_t tag[4];
};

class GhostScanCallbacks final : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    if (!gBle || !dev->haveManufacturerData()) return;
    const std::string raw = dev->getManufacturerData();
    if (raw.size() != sizeof(BleHint)) return;

    BleHint hint{};
    memcpy(&hint, raw.data(), sizeof(hint));
    if (hint.magic[0] != 'G' || hint.magic[1] != 'L') return;
    if (hint.version != PROTOCOL_VERSION || hint.role != 0) return;
    if (hint.networkId != ghostpair::NETWORK_ID) return;

    uint8_t expected[4]{};
    GhostBle::signHint(hint.slot, hint.msIntoSlot, expected);
    if (memcmp(expected, hint.tag, 4) != 0) return;

    // We cannot directly touch Wi-Fi state inside the NimBLE callback; just
    // hand the signed timing hint to the main loop.
    gBle->syncSlot_ = hint.slot;
    gBle->syncMs_ = hint.msIntoSlot;
    gBle->syncReady_ = true;
    NimBLEDevice::getScan()->stop();
  }
};

static GhostScanCallbacks scanCallbacks;

void GhostBle::signHint(uint32_t slot, uint16_t msIntoSlot, uint8_t out[4]) {
  uint8_t data[8];
  data[0] = uint8_t(ghostpair::NETWORK_ID >> 8);
  data[1] = uint8_t(ghostpair::NETWORK_ID);
  data[2] = uint8_t(slot >> 24);
  data[3] = uint8_t(slot >> 16);
  data[4] = uint8_t(slot >> 8);
  data[5] = uint8_t(slot);
  data[6] = uint8_t(msIntoSlot >> 8);
  data[7] = uint8_t(msIntoSlot);

  uint8_t digest[32]{};
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_hmac(info, ghostpair::HOP_KEY, sizeof(ghostpair::HOP_KEY),
                  data, sizeof(data), digest);
  memcpy(out, digest, 4);
}

bool GhostBle::verifyHint(uint32_t slot, uint16_t msIntoSlot, const uint8_t tag[4]) {
  uint8_t expected[4]{};
  signHint(slot, msIntoSlot, expected);
  return memcmp(expected, tag, 4) == 0;
}

void GhostBle::begin(uint8_t role) {
  if (!ENABLE_BLE_RESCUE || GHOST_PAIR_CONFIGURED != 1) return;
  role_ = role;
  gBle = this;

  NimBLEDevice::init(role == 0 ? "GHOSTLINK-A" : "GHOSTLINK-B");
  started_ = true;

  if (role_ == 0) {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->setName("GHOSTLINK");
    adv->start();
  } else {
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&scanCallbacks, false);
    scan->setActiveScan(false);
    scan->setInterval(97);
    scan->setWindow(47);
    scan->setMaxResults(0);
  }
}

void GhostBle::updateAdvertisement(uint32_t slot, uint16_t msIntoSlot) {
  BleHint hint{};
  // 0xFFFF is a private/testing company identifier in this hobby protocol; the
  // magic/network/tag fields are what GHOSTLINK validates.
  hint.companyLo = 0xFF;
  hint.companyHi = 0xFF;
  hint.magic[0] = 'G';
  hint.magic[1] = 'L';
  hint.version = PROTOCOL_VERSION;
  hint.role = 0;
  hint.networkId = ghostpair::NETWORK_ID;
  hint.slot = slot;
  hint.msIntoSlot = msIntoSlot;
  signHint(slot, msIntoSlot, hint.tag);

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setManufacturerData(reinterpret_cast<uint8_t*>(&hint), sizeof(hint));
  adv->refreshAdvertisingData();
}

void GhostBle::maybeStartScan(uint32_t lostForMs) {
  if (lostForMs < BLE_RESCUE_AFTER_MS) return;
  const uint32_t now = millis();
  if (now - lastScanStartMs_ < BLE_SCAN_GAP_MS) return;

  NimBLEScan* scan = NimBLEDevice::getScan();
  if (!scan->isScanning()) {
    lastScanStartMs_ = now;
    scan->start(BLE_SCAN_MS, false, true);
  }
}

void GhostBle::loop(uint8_t role, uint32_t slot, uint16_t msIntoSlot,
                    bool wifiSynced, uint32_t lostForMs) {
  if (!started_) return;
  const uint32_t now = millis();

  if (role == 0) {
    if (now - lastAdvRefreshMs_ >= BLE_ADV_REFRESH_MS) {
      updateAdvertisement(slot, msIntoSlot);
      lastAdvRefreshMs_ = now;
    }
  } else if (!wifiSynced) {
    maybeStartScan(lostForMs);
  }
}

bool GhostBle::consumeSync(uint32_t& slot, uint16_t& msIntoSlot) {
  if (!syncReady_) return false;
  noInterrupts();
  slot = syncSlot_;
  msIntoSlot = syncMs_;
  syncReady_ = false;
  interrupts();
  return true;
}

} // namespace ghost
