# GHOSTLINK protocol v3

This is a closed, provisioned pair. A and B both transmit and receive text
directly over ESP-NOW. A alone owns the hop clock. There is no infrastructure,
forwarding, device enrollment over the air, or firmware propagation.

## Timing and rendezvous

A slot lasts 1800 ms. Normal slot channel selection is
1 + HMAC-SHA256(HOP_KEY, big-endian NETWORK_ID || slot)[0] mod 11.
Every eighth slot uses slot=0 for that calculation, producing the same
pair-specific home channel. A starts on home at slot zero.

A repeats SYNC approximately every 180 ms. B initially walks channels with
300 ms plus a boot-specific 0..46 ms dwell offset. If it has not synchronized
after 9 seconds, it parks on home. A returns there every 14.4 seconds.
A valid BLE hint can temporarily move B to a candidate channel for 700 ms;
it does not establish clock sync or peer liveness.

The first and last 60 ms of a slot are transmit guards. A sends messages in the
first half and B in the second, with an additional guard at the midpoint.
ACKs and A's SYNC may use either half, outside boundary guards. This reduces
simultaneous data sends; it is not collision-free TDMA. ESP-NOW failures and
missing application ACKs are recovered by retry.

Only one ESP-NOW send is outstanding. The completion callback posts a queue
event; the main loop clears the in-flight state. Channel changes wait for that
completion. A stalled driver callback requires board reset; this release does
not reset an active Wi-Fi driver from inside a callback.

## Wire packet and authentication

The packed little-endian packet in ghost_protocol.h fits the 250-byte
ESP-NOW v1 payload limit. Fields are:

- GHST magic, protocol version 3, type, sender role, reserved flags
- pair network ID
- random nonzero sender boot session and increasing frame number
- logical message sequence, ACK sequence, ACK target session
- anchor slot and milliseconds within the slot
- text byte count, 161-byte text buffer (160 bytes plus zero terminator)
- 16-byte HMAC-SHA256 tag

ESP-NOW uses the pair PMK/LMK for encrypted unicast. Only the configured peer
MAC is accepted. The packet authentication key is derived by HMAC-SHA256
from HOP_KEY with the domain "GHOSTLINK packet v3". The 128-bit tag covers
every field before the tag, including metadata and the entire text buffer.

Malformed packets, wrong versions/roles/networks, failed authentication and
replayed frames are discarded before updating link state. A 32-frame replay
window allows recent out-of-order frames but only the newest frame can update
the clock. Four retired peer boot identities are remembered in RAM. This is
not persistent freshness protection against recorded traffic across reboots.

## Delivery

Up to eight outbound messages live in a FIFO. Only the front message is
in-flight. Every retry gets a fresh frame number while retaining the same
logical message sequence and boot session.

The receiver remembers 32 accepted (session, sequence) pairs. For a new message,
it first enqueues the text; only success permits ACK and duplicate-cache
insertion. A full application queue therefore causes the sender to retry.
Duplicates are ACKed without re-enqueueing. ACKs identify both the sender's
boot session and the logical message sequence.

The sender displays delivery only after that application ACK. MAC-layer send
success is not delivery. ACKs from an earlier sender boot cannot clear its new
outbox. Six attempts per burst are followed by a five-second rest, with no
automatic message deletion. While disconnected, messages wait in RAM.

## Clock handling

All elapsed millisecond arithmetic uses unsigned subtraction to support timer
wrap. Receive callbacks timestamp packets before queueing them. The follower
sets its clock from the anchor timestamp and the receive timestamp, then
advances it for processing delay. Every newest authenticated A packet can
provide synchronization, not just SYNC.

The timing estimate still contains radio scheduling and on-air latency; the
guard is a practical allowance, not measured clock precision. Physical tests
must confirm the margin on the actual boards.

## BLE advisory payload

Nonconnectable legacy advertisements contain flags and a manufacturer payload:
test company ID 0xFFFF, GL magic, version, role, network ID, slot, phase and an
8-byte HMAC tag. The domain "GHOSTLINK BLE v3" derives a separate signing key.
All preceding hint fields are authenticated. No chat text or key material is
advertised. The manufacturer ID is for this hobby prototype, not a Bluetooth
SIG product identifier allocation.

A FreeRTOS queue transfers hints between the BLE callback and main loop.
Hints older than 250 ms in that queue are discarded. B scans only while
unsynchronized and stops scanning once authenticated ESP-NOW synchronization
returns. Captured signed hints can be replayed to suggest a stale channel;
they cannot establish LINK or move an already synchronized follower.

## Sources

- [Espressif ESP-NOW v4.4.8](https://docs.espressif.com/projects/esp-idf/en/v4.4.8/esp32/api-reference/network/esp_now.html):
  application ACKs, callback queues, peer channel zero and rate selection.
- [Espressif coexistence](https://docs.espressif.com/projects/esp-idf/en/v5.2-beta1/esp32/api-guides/coexist.html):
  Wi-Fi and BLE share one radio.
