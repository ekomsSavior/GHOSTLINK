# GHOSTLINK

Two ESP32 CYDs, two-way text, no router, SIM, phone, server or internet at runtime.

A is the **anchor** and B is the **hunter**. They follow a pair-specific channel
schedule, exchange encrypted ESP-NOW messages, and automatically find each
other again after a radio outage. Both boards can send and receive.

**A fresh clone contains the complete project. No previous GHOSTLINK release
is needed.** Build both boards from the same checkout and pair them once.

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

## Clone and build

You need Git, Python 3, and PlatformIO with the `pio` command available
(or the PlatformIO IDE terminal).

```sh
git clone https://github.com/ekomsSavior/GHOSTLINK.git
cd GHOSTLINK
pio run -e ghost-a -e ghost-b
```

You can also clone the same project from
[Gitea](https://git.churchofmalware.org/ek0mssavi0r/GHOSTLINK).
Choose either repository; you only need one checkout. PlatformIO downloads
the pinned ESP32 toolchain and libraries on the first build. Internet is
needed for those downloads, but the boards communicate entirely offline.

## Read the two board addresses

Connect both boards with USB data cables. Replace the example serial ports
below with their actual ports (`pio device list` shows connected devices).

Flash board A and read its station MAC:

```sh
pio run -e ghost-a -t upload --upload-port /dev/ttyUSB0
pio device monitor --port /dev/ttyUSB0
```

Exit the monitor with Ctrl+C, then flash board B and read its station MAC:

```sh
pio run -e ghost-b -t upload --upload-port /dev/ttyUSB1
pio device monitor --port /dev/ttyUSB1
```

The boards show **SETUP** until paired. Record both MAC addresses, then
exit the monitor before continuing.

## Pair and flash

Generate **one** configuration, then use it for both builds:

```sh
python3 tools/make_pair.py \
  --a AA:BB:CC:DD:EE:01 \
  --b AA:BB:CC:DD:EE:02 \
  > include/ghost_pair_private.h
pio run -e ghost-a -t upload --upload-port /dev/ttyUSB0
pio run -e ghost-b -t upload --upload-port /dev/ttyUSB1
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

## License

MIT. Free to use, modify and share.
