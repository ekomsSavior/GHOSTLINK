# GHOSTLINK

Offline text messaging between ESP32 CYDs. No router, SIM, phone, server,
external radio, or internet connection is needed while communicating.

Two CYDs work as the chat endpoints **A** and **B**. Additional CYDs can run
as relays, automatically discover their neighbors, and carry encrypted messages
between the endpoints. Relays use the same onboard ESP32 radio and screen;
no additional components are required.

Messages wait in onboard flash during an outage and resume when a path becomes
available. The destination confirms delivery. Text is packed into fewer bytes
when possible, then restored at the destination.

**A fresh clone contains the complete project. No previous GHOSTLINK release
is needed.** Build every participating board from this checkout.

## Hardware

Use a **2.8-inch ESP32-2432S028R CYD** with the original ESP32,
XPT2046 resistive touch, and 4 MB flash. Select the profile for its display:

| Display variant | Board A | Board B | Relay |
| --- | --- | --- | --- |
| Original ILI9341 | `ghost-a` | `ghost-b` | `ghost-relay` |
| ST7789, commonly USB-C + Micro-USB | `ghost-a-st7789` | `ghost-b-st7789` | `ghost-relay-st7789` |

Both profiles use a 320 × 240 landscape screen. The commands below use the
original profile; for an ST7789 board, use its `-st7789` environment in every
build and upload command.

| Peripheral | Pins | SPI |
| --- | --- | --- |
| Display | MOSI 13, MISO 12, CLK 14, CS 15, DC 2, backlight 21 | HSPI |
| Touch | MOSI 32, MISO 39, CLK 25, CS 33, IRQ 36 | VSPI |

CYD revisions vary. Display settings are in `include/User_Setup_Ghostlink.h`
and touch calibration is in `include/ghost_config.h`.

## Clone and build

You need Git, Python 3, and PlatformIO with the `pio` command available
(or the PlatformIO IDE terminal).

```sh
git clone https://github.com/ekomsSavior/GHOSTLINK.git
cd GHOSTLINK
pio run -e ghost-a -e ghost-b
```

For the dual-USB ST7789 boards, build with:

```sh
pio run -e ghost-a-st7789 -e ghost-b-st7789
```

The same project is available on
[Gitea](https://git.churchofmalware.org/ek0mssavi0r/GHOSTLINK).
Choose either repository; only one checkout is needed. PlatformIO downloads
the pinned toolchain and libraries for the first build. The boards operate
offline once flashed.

## Read the endpoint addresses

Connect your CYDs with USB data cables. Use `pio device list` to find their
serial ports and replace the example ports below.

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

The boards show **SETUP** until configured. Record both MAC addresses, then
exit the monitor.

## Configure and flash the endpoints

Generate **one** configuration for your two actual MAC addresses:

```sh
python3 tools/make_pair.py \
  --a AA:BB:CC:DD:EE:01 \
  --b AA:BB:CC:DD:EE:02 \
  > include/ghost_pair_private.h
pio run -e ghost-a -t upload --upload-port /dev/ttyUSB0
pio run -e ghost-b -t upload --upload-port /dev/ttyUSB1
```

Replace those example MACs and serial ports. A and B must use the same
configuration. Each endpoint checks its own address against its assigned role.
Keep this file for future builds of this network; generating another one
creates different keys. The private file is ignored by Git.

## Add CYD relays

Export relay membership from the existing endpoint configuration:

```sh
python3 tools/make_relay.py include/ghost_pair_private.h \
  > include/ghost_relay_private.h
pio run -e ghost-relay -t upload --upload-port /dev/ttyUSB2
```

Flash `ghost-relay` to each additional CYD, changing the port for each board.
No relay MAC list is needed. Relays discover other configured CYDs automatically
and display their neighbor count and stored packets. They can forward and store
messages without the endpoints' chat decryption key.

Keep both private configuration files and configured firmware private. Share
only the relay configuration with someone provisioning a relay for your network.
The relay build uses `ghost_relay_private.h` and does not include the endpoint
configuration. Public setup builds contain no usable network keys.

## Use

Type on A or B and press **SEND**. `123` changes to digits and punctuation;
`ABC` returns to letters. Tap the left or right half of the chat area to browse
older or newer messages.

- **SETUP**: configure the board before messaging.
- **ERROR**: check the serial console for the configuration, storage, or radio error.
- **HUNT**: no participating neighbor is currently in contact.
- **MESH**: at least one participating neighbor was heard recently.
- **N:n**: number of neighbors currently in contact.
- **Q:n**: outgoing messages awaiting destination confirmation.
- **.** beside a message: queued or awaiting confirmation.
- **+**: the destination accepted the message; this is not a human read receipt.

A neighbor connection alone does not confirm a path to the destination. The
**+** receipt confirms delivery. Pending outgoing messages survive a restart.
When the eight-message outbox is full, SEND keeps your text in the composer.

At 115200 baud, enter plain text to send from either endpoint. `/status` shows
radio, queue, and channel information; `/neighbors` lists nearby participating
CYDs; `/help` lists commands. Messages can contain up to 160 UTF-8 bytes.

## Channels and mesh coverage

A coordinates a shared hopping schedule, which relays pass onward. Every
24 seconds the schedule returns to the network's home channel so separated
nodes can meet again. BLE supplies nearby discovery hints. The radios observe
surrounding Wi-Fi beacon activity and coordinate channel preferences, while
continuing to sample other channels.

Two CYDs need a direct radio path. Additional CYDs extend coverage by receiving
and retransmitting messages. A moving relay can carry a stored message between
contacts. All forwarding devices must run GHOSTLINK with this network's relay
configuration. The built-in LR mode handles the radio link; channel selection
and compact messages reduce interference exposure and airtime.

## License

MIT. Free to use, modify and share.
