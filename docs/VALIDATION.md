# Engineering validation — 2026-10-15

Target: GHOSTLINK 0.4.0, classic 4 MB ESP32-2432S028R CYD.
Toolchain: PlatformIO 6.2.0, espressif32 7.1.3, Arduino ESP32 2.0.17,
TFT_eSPI 2.5.43, NimBLE-Arduino 2.3.6, pinned XPT2046 source revision.

| Check | Result |
| --- | --- |
| A, B, relay configured firmware builds | PASS |
| A, B, relay setup builds from tracked source only | PASS |
| Host protocol/mesh suite | PASS |
| Address and undefined-behavior sanitizers | PASS |
| Pair generator and relay export key separation | PASS |
| Git whitespace check | PASS |

The host suite executes the firmware's portable protocol, codec, envelope,
queue, replay, neighbor, and clock implementations. It covers:

- Every text length from 1 to 160 bytes, packed and raw byte content, exact
  round trips, unsupported characters, and variable wire sizes.
- AES-256-GCM known-answer vector, altered ciphertext/tag/address/sequence,
  invalid receipts, and inability of a relay key to decrypt endpoint messages.
- Every single-byte mutation of a valid frame, truncated frames, and 10,000
  malformed frame/envelope inputs.
- Timer/frame wrap, retired boot identities, neighbor expiry, delayed clock
  processing, multi-hop clock forwarding, and stale-pulse loop rejection.
- Three-node channel simulation with 30% HELLO loss, missed schedule activation,
  home-channel recovery, and coordinator reboot. No direct A-to-B edge exists.
- Full origin/inbox/forwarding queues, ACK admission when data storage is full,
  durable snapshot restoration, duplicate receipts, malformed persisted state,
  hop limits, and refusal to replace ciphertext under an existing message ID.
- 100 two-way simulated deliveries through relays with 40% data/ACK packet loss,
  disconnected contacts, a relay restart, an alternative relay path, and a
  complete temporary outage. Endpoints never have a direct simulated edge.

Host cryptographic calls use OpenSSL; the ESP32 builds use mbedTLS. Both use
identical envelope, key-derivation, nonce, and associated-data code. Native
simulations model packet delivery/loss and channel matching, not RF propagation,
Wi-Fi/BLE scheduling, ESP-NOW driver callbacks, NVS hardware behavior, or flash
power-failure timing. They are not proof of over-the-air range or durability.

Run the host checks with Python 3, g++, and the OpenSSL development headers:

```sh
python3 tools/test_host.py
```

For the sanitizer run:

```sh
g++ -std=c++17 -Wall -Wextra -Werror -DGHOST_HOST_TEST \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I include -I src tests/protocol_test.cpp -lcrypto -o /tmp/ghostlink-tests
/tmp/ghostlink-tests
```

Configured builds use approximately 21.7% of static RAM and 56.2% of their
1856 KiB application partition. Runtime heap/stack usage still needs measurement.
TFT_eSPI's optional built-in touch warning is expected because this project uses
XPT2046_Touchscreen on a separate SPI peripheral.

No /dev/ttyUSB* or /dev/ttyACM* ESP32 device was connected during this work.
Use BENCH_TEST.md for the physical tests. All synthetic private configuration
headers were removed after compilation; no test keys or configured firmware
are included in the published source. README intentionally contains setup and
operation instructions rather than this engineering record.

## Serial upload layout regression

The first board upload exposed a mismatch: the partition table expected the app
at 0x30000 while the Arduino uploader wrote it at 0x10000 (and the OTA initializer
at 0xE000 instead of 0x20000). This produced a bootloader reset loop before the
application banner. The table now uses the uploader addresses and puts 128 KiB
of NVS after the application slots. The build guard reads the actual PlatformIO
app/extra-image addresses; regression tests reject wrong app, OTA, and overlap
layouts. Build success alone is not a physical boot or radio test.

The corrected table passed builds for ghost-a, ghost-b, and ghost-relay. Four
flash-layout regression tests passed, and the generated A/B partition binaries
were decoded to confirm the app, OTA data, and NVS addresses. The paired-board
helper was syntax/import checked; its hardware result must come from a real run.
