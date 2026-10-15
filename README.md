# GHOSTLINK

**Free, open-source, two-CYD off-grid messaging.**

GHOSTLINK turns two common ESP32-2432S028R "Cheap Yellow Display" boards into a tiny infrastructure-free text link. No router, phone, cloud account, server, SIM, or Internet connection is required.

The vibe is autonomous and self-healing: the anchor keeps moving through a pair-specific Wi-Fi channel schedule; the hunter searches when the link disappears, catches an authenticated sync frame, and locks back on. BLE is a low-duty rescue beacon. The system is intentionally restricted to the two peers you configure.

## v0.2 features

- two-node ESP-NOW encrypted unicast
- pair-specific HMAC-derived Wi-Fi channel hopping
- autonomous hunt / resynchronization after link loss
- signed BLE rendezvous/rescue hints
- GHOST-LR (Espressif LR PHY) with runtime fallback
- explicit Wi-Fi max TX-power request with driver read-back
- touchscreen QWERTY keyboard
- chat history
- sequence numbers, application ACKs, retry, duplicate suppression
- RSSI / channel / HUNT / LINK status
- same source tree builds node A and node B
- MIT licensed

## Expected hardware

This first target is the common **ESP32-2432S028R 2.8-inch CYD**:

- ILI9341 320x240 display
- XPT2046 resistive touch
- ESP32-WROOM-32

Display pins used by the common board are MOSI 13, MISO 12, SCLK 14, CS 15, DC 2, BL 21. Touch is on its separate pin group: CLK 25, MOSI 32, MISO 39, CS 33, IRQ 36.

CYD clones vary. If the screen is blank/inverted or touch is mirrored, adjust `include/User_Setup_Ghostlink.h` and the four calibration values in `include/ghost_config.h`.

## Build tool

PlatformIO is the easiest route.

```bash
cd GHOSTLINK
pio run -e ghost-a
pio run -e ghost-b
```

## First boot: get both MAC addresses

Leave `GHOST_PAIR_CONFIGURED` at `0` and flash each board once. Each unit displays and prints its Wi-Fi station MAC.

```bash
pio run -e ghost-a -t upload
pio device monitor
```

Record board A's MAC. Then do the same with board B.

## Generate a private pair config

Do **not** use shared public keys. Generate unique keys for every pair:

```bash
python3 tools/make_pair.py \
  --a AA:BB:CC:DD:EE:01 \
  --b AA:BB:CC:DD:EE:02 \
  > include/ghost_pair.h
```

Keep your generated `ghost_pair.h` private if confidentiality matters.

## Flash the pair

Flash A:

```bash
pio run -e ghost-a -t upload
```

Flash B:

```bash
pio run -e ghost-b -t upload
```

A becomes the hop-clock anchor. B becomes the hunter/follower.

## On-screen states

- `SETUP` - no pair keys/MACs yet
- `HUNT` - follower is walking channels looking for A
- `SYNC` - synchronized but recent peer traffic is missing
- `LINK` - recent encrypted peer traffic received

Outgoing messages show:

- `.` waiting for ACK
- `+` delivered
- `!` retries exhausted

## Security model

ESP-NOW encrypted unicast uses a pair-specific PMK/LMK. GHOSTLINK also checks the configured source MAC and protocol fields before accepting packets. The channel sequence comes from a separate pair-specific HMAC key.

Channel hopping is **not** treated as cryptography. It exists for resilience, link recovery, and the project aesthetic.

BLE carries no chat content. Its rendezvous hint is authenticated with a truncated HMAC so random BLE advertisements cannot resynchronize the Wi-Fi state machine.

## What GHOSTLINK deliberately does not do

GHOSTLINK does not self-replicate, compromise devices, connect to arbitrary access points, scan strangers' traffic, or turn unknown ESP32s into peers. The worm-like behavior is strictly the self-healing state machine between the two configured boards.

## Current rough edges

This is the first hardware build. Before calling it stable, test on both physical units:

1. display orientation/colors
2. touch calibration
3. encrypted message delivery on a fixed nearby pair
4. hopping over several minutes
5. power-cycle B and confirm HUNT reacquires A
6. power-cycle both and confirm recovery
7. BLE rescue/coexistence behavior

If BLE causes instability on a particular Arduino core, set `ENABLE_BLE_RESCUE = false` in `include/ghost_config.h`; ESP-NOW hunt/resync remains fully functional.

## License

MIT. Build it, fork it, teach with it, improve it, and keep it free.

## v0.2 GHOST-LR + BLE rescue

v0.2 adds a dedicated long-range radio profile for two ESP32 CYDs. When
`ENABLE_WIFI_LR` is enabled, GHOSTLINK requests Espressif's proprietary
`WIFI_PROTOCOL_LR` PHY on both nodes. No extra radio chip is required.

The follower still uses the secret-derived ESP-NOW channel schedule. If it
loses the anchor it enters HUNT mode, sweeps channels, and simultaneously uses
low-duty-cycle BLE advertisements as a signed rendezvous/control plane. BLE
never carries chat text, PMK/LMK material, or the hopping key.

Important: BLE rescue improves reacquisition; it does not extend the RF range
of ESP-NOW. GHOST-LR and antenna/environment quality are what improve distance.

Configuration lives in `include/ghost_config.h`:

```cpp
constexpr bool ENABLE_WIFI_LR = true;
constexpr bool LR_ONLY_PHY = true;
constexpr int8_t WIFI_MAX_TX_POWER_QDBM = 80; // requests 20 dBm max
```

The Wi-Fi driver may clamp transmit power based on chip and regulatory/country
configuration. Stay within the radio rules for your location.
