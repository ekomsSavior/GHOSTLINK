# GHOSTLINK 0.3

Two ESP32 CYDs, two-way text, no router, SIM, phone, server or internet at runtime.

A is the **anchor** and B is the **hunter**. They follow a pair-specific channel
schedule, exchange encrypted ESP-NOW messages, and automatically find each
other again after a radio outage. Both boards can send and receive.

## What changed in 0.3

- Eight-message RAM outbox: compose while disconnected; transmission resumes
  when the peer returns. Messages are kept while the board stays powered.
- Delivery confirmation only after the peer accepts the message into its
  application queue. Lost ACKs cause retries without repeated chat lines.
- Random boot identities, ACKs tied to the sender's boot, a duplicate cache and
  a bounded frame replay window.
- Authenticated packet metadata and text inside ESP-NOW encrypted unicast.
- Separate A/B message transmission windows and a guard around channel changes.
  One ESP-NOW transmission at a time; callbacks only pass events to queues.
- Receive timestamps account for time spent waiting in the main loop.
- A pair-specific rendezvous channel every eighth 1.8-second hop. After nine
  seconds of hunting, B waits there. This recovery does not depend on BLE.
- BLE hints suggest a channel, then require authenticated Wi-Fi traffic before
  showing synchronization or LINK. Wi-Fi and BLE share the ESP32 radio.
- Explicit ESP-NOW LR 250K transmit rate, checked initialization and power-cap
  readback. Both boards use the same profile; a configuration error is visible.
- Real 123/ABC keyboard pages, touch press/release handling, outbox count and
  serial message/status commands.
- Separate HSPI display and VSPI touch peripherals, pinned build dependencies,
  and an ignored private pair-config file.

**Both boards must be upgraded together. Protocol v3 is incompatible with v0.2.**

## Hardware

The current display target is the classic **ESP32-2432S028R 2.8-inch CYD** with
ESP32-WROOM-32, ILI9341 display and XPT2046 resistive touch.

| Peripheral | Pins | SPI |
| --- | --- | --- |
| Display | MOSI 13, MISO 12, CLK 14, CS 15, DC 2, backlight 21 | HSPI |
| Touch | MOSI 32, MISO 39, CLK 25, CS 33, IRQ 36 | VSPI |

CYD revisions vary. Display configuration is in
`include/User_Setup_Ghostlink.h`; touch calibration is in
`include/ghost_config.h`. Other ESP32 boards require board/display
adaptation; this is not a universal ESP32-family build.

## Build and get the two MAC addresses

Install Python 3 and PlatformIO (the tested CLI version is in
`requirements-dev.txt`), then:

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements-dev.txt
.venv/bin/pio run -e ghost-a -e ghost-b
.venv/bin/pio run -e ghost-a -t upload --upload-port /dev/ttyUSB0
.venv/bin/pio device monitor --port /dev/ttyUSB0
```

The initial build deliberately has no pair keys. It shows **SETUP** and prints
the board's station MAC. Read both actual MACs before generating a pair.

## Pair and flash

Generate **one** configuration, then use it for both builds:

```sh
python3 tools/make_pair.py \
  --a AA:BB:CC:DD:EE:01 \
  --b AA:BB:CC:DD:EE:02 \
  > include/ghost_pair_private.h
.venv/bin/pio run -e ghost-a -t upload --upload-port /dev/ttyUSB0
.venv/bin/pio run -e ghost-b -t upload --upload-port /dev/ttyUSB1
```

Replace those example MACs and serial ports. The firmware verifies that its
own MAC matches its A/B role. The private header is ignored by Git. Do not
regenerate it between boards: that would give them different keys.

Builds contain the pair keys once configured. Keep configured firmware and
the private header private. The public setup builds contain no usable pair.

## Use

Type on either touchscreen and press SEND. `123` changes to digits and
punctuation; `ABC` returns to letters. Each tap inserts one character.
Messages wrap in full. Tap the left/right half of the chat area to browse
older/newer entries (24 messages retained while powered).

- **SETUP**: no pair provisioned.
- **ERROR**: radio initialization or local MAC validation failed; check serial.
- **HUNT**: B is searching for A.
- **SYNC**: A has its clock but has not recently heard B.
- **LINK**: authenticated ESP-NOW traffic was recently received.
- **Q:n**: number of queued messages.
- **.** beside a message: waiting for the peer's application ACK.
- **+**: peer accepted the message. This is not a human read receipt.

At 115200 baud, type plain text followed by Enter to send. `/status`
prints channel, sync/link state, queue depth and packet counters;
`/help` prints the console commands. Messages are limited to 160 bytes.

If the eight-message outbox is full, SEND retains the composer and the serial
console reports the reason. After six attempted sends without an ACK, the
sender rests for five seconds and retries. Disconnects do not discard messages.

## Range and recovery

LR uses the existing ESP32 radio; no additional chip is needed. Actual distance
depends on antennas, placement, obstructions and interference. Hopping and BLE
help recovery, not RF propagation. A cannot relay through absent third nodes.

After deep hunt begins, A offers a rendezvous slot every **14.4 seconds**.
That is a scheduled opportunity, not a guaranteed recovery deadline under loss
or interference. BLE can shorten discovery nearby. Disable
`ENABLE_BLE_RESCUE` to compare Wi-Fi-only recovery on the bench.

## Validation and limitations

Both firmware roles have been compiled in setup and synthetic paired
configurations. Host tests cover timing wrap, delayed sync processing, packet
validation, stale ACKs, peer reboots, duplicate frames, receive-queue
backpressure and 400 simulated deliveries with data/ACK loss and outages.
They do not simulate RF propagation or prove over-the-air performance.

Run the protocol tests:

```sh
python3 tools/test_host.py
```

No physical ESP32 was attached during this update. Follow
[the bench checklist](docs/BENCH_TEST.md) before relying on the link.

Outbox, chat history and duplicate/replay state are RAM-only. Power loss clears
them; a receiver reboot after a lost ACK can display the retried message again.
There is no forward secrecy or persistent anti-replay guarantee across reboots.
Keys are compiled into firmware; this build does not enable secure boot or
flash encryption. Serial diagnostics expose local chat text. RSSI is unavailable
through the pinned Arduino 2.x ESP-NOW receive callback and displays "--" there.

See [protocol details](docs/PROTOCOL.md) and
[source recovery provenance](docs/SOURCE_PROVENANCE.md).

## License

MIT. Free to use, modify and share.
