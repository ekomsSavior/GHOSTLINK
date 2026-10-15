# GHOSTLINK mesh protocol v4

This is a provisioned network with two chat endpoints (A and B) and optional
CYD relays. It uses active ESP-NOW transmissions at Espressif LR 250K, with
ordinary Wi-Fi reception enabled for activity sampling. It implements no
ambient backscatter, signal hijacking, arbitrary-router forwarding, RF energy
harvesting, or firmware propagation. Compression reduces airtime, not path loss.

## Membership and forwarding

All participating nodes share a network ID and 256-bit membership key (HOP_KEY).
Signed HELLO frames discover up to 16 live neighbors without a relay MAC list.
Neighbor records expire after 10 seconds without fresh frames. Each keeps a
32-frame replay window and four retired sender boot IDs. Only authenticated
frames allocate neighbor records. The callbacks just enqueue bounded events;
authentication and storage run on the application loop.

Forwarding is bounded flooding, not an IP routing protocol or a maintained
unicast route table. Alternate paths work because each participating relay
can retransmit a message. A packet has a maximum of eight radio transmissions
from its origin. A longer duplicate path cannot increase its remaining hop
budget. Duplicate IDs with different ciphertext cannot overwrite a cache entry.

Each node has an eight-message origin outbox, twelve forwarding slots, and an
eight-message receive inbox. Relays retain ciphertext for 20 minutes of uptime
since admission (that retention timer starts again after reboot). Origins keep
messages until an authenticated destination receipt. Retry intervals are about
3.5 seconds at origins and 4.5 seconds at relays, plus jitter, subject to shared
radio airtime and queue scheduling. Discovery of a new/rebooted neighbor wakes
pending transmissions. These are small-network bounds, not large-mesh capacity
claims. Independent messages can arrive out of order across different paths.

ACKs can evict forwarding data when the cache is full. A relay retires matching
data upon a membership-authenticated ACK and retains the ACK as a tombstone for
20 minutes. It sends an ACK up to three times, then again upon a new neighbor
or matching data retry. Only the destination/source endpoints can validate the
end-to-end ACK tag. Consequently relay membership is trusted for availability:
a member can suppress forwarding or impersonate transport metadata, but cannot
forge an endpoint's delivery receipt or decrypt chat without its separate key.

## Wire format and cryptography

All wire integers are little-endian. Frames contain a packed 24-byte header:
magic GHST, version 4, type, network ID, transmitter MAC, random 32-bit transmitter
boot ID, increasing 32-bit frame number, hop count, and body length. A 16-byte
truncated HMAC-SHA256 covers that header and the entire variable-length body.
The transport key is derived with the domain `GHOSTLINK mesh transport v4`.
The authenticated transmitter MAC must equal the ESP-NOW callback source.

ESP-NOW broadcast itself is unencrypted: ESP-NOW does not support CCMP-encrypted
broadcast. The application supplies encryption. A message envelope has a
28-byte header containing type, codec, original/encoded lengths, source and
destination MACs, a 64-bit message session, and a 32-bit sequence. It is followed
by exactly the encoded length of ciphertext and a 16-byte AES-256-GCM tag.
The largest frame is 244 bytes and fits the pinned ESP-NOW v1 250-byte limit.
There is no fixed-size 160-byte padding on short messages.

Endpoint LMK is the 128-bit secret input to HMAC-SHA256 key derivation; deriving
256-bit AES keys does not increase its 128-bit entropy. A chat master uses the
domain `GHOSTLINK chat master v4`; source and destination MACs derive separate
direction keys. The GCM nonce is the random 64-bit message session followed by
the 32-bit sequence. Version, network ID, and immutable envelope header are
associated data. Retries and relays preserve identical ciphertext; they do not
reuse a nonce to encrypt different plaintext. Sequence exhaustion refuses new
messages. Random-session collision probability remains a probabilistic bound.

Receipts reverse the endpoint addresses and identify the original session and
sequence. They use a separate HMAC key (`GHOSTLINK receipt v4`) rather than GCM,
so they cannot collide with a data nonce. A relay export includes membership,
network ID, and endpoint addresses, but zeros PMK/LMK and excludes the endpoint
private header. PMK is retained in generated configurations for source-format
continuity; it is not used for this broadcast transport.

The text codec packs this 64-character alphabet into six bits per character:
space, a-z, A-Z, 0-9, period. It is selected only when every byte is supported
and the encoded result is shorter. Otherwise the original UTF-8 bytes are sent.
Decoding is bounded by the authenticated original length; malformed lengths,
invalid padding, embedded zero bytes, and unknown codec values are rejected.

## Rendezvous and coordinated channel preferences

A owns the clock. Slots last 3000 ms, with 150 ms transmit guards at both ends.
Every eighth slot uses a fixed home channel derived from network membership.
Disconnected nodes wait on home and can discover, exchange, and store messages
even while A is absent. A home slot is an opportunity, not a recovery deadline.

A sends increasing root pulses in HELLO messages. Synchronized nodes advertise
the same pulse with their current phase and distance from A. A receiver adjusts
for main-loop queue delay. Only a newer root pulse refreshes synchronization;
recirculating an old pulse cannot keep an isolated cluster synchronized.
A root reboot creates a new boot identity; recent retired roots cannot rewind
synchronization. Propagation delay and oscillator drift require RF measurement.

Nodes count nearby Wi-Fi beacons/probe responses by channel, without storing
other users' payloads. Observations are normalized by dwell time and smoothed.
These counts are an activity proxy, not a calibrated spectrum analyzer or full
channel-utilization measurement. A combines its observations with those of its
direct neighbors. Once every channel has a sample, it may propose the three
quietest channels if their mean score improves by more than 25 percent. Plan
reviews occur at most once per minute, with activation 16 slots in the future.
Every HELLO carries current and pending masks and the activation slot. Receivers
that miss updates recover through the unchanged home channel.

Non-home slots divisible by four probe channels outside the preferred mask;
other slots use a deterministic shared channel choice from that mask. No node
independently changes the network schedule. Disable ENABLE_CHANNEL_ADAPTATION
for a controlled comparison against the full channel mask.

BLE advertisements contain only a channel hint and network/version metadata,
authenticated with a separately derived key and 64-bit tag. All synchronized
nodes may advertise. Unsynchronized nodes scan briefly. Hints expire locally
and only suggest a temporary candidate channel; fresh authenticated Wi-Fi root
pulses establish synchronization. Captured BLE hints can be replayed, and BLE
shares the ESP32 radio with Wi-Fi.

## Durable state and failure handling

An NVS blob atomically commits outgoing ciphertext, forwarding ciphertext,
unconsumed inbox envelopes, and the last 32 received message identities. The
snapshot binds to network ID, membership key, endpoint key, addresses, role,
and protocol. Failed writes roll RAM changes back. A new message is acknowledged
only after its inbox admission and duplicate receipt are saved. Provisioning a
new identity starts a new queue; corrupt/incompatible stored state stops radio
initialization rather than silently dropping it. The 4 MB partition table
allocates 92 KiB to NVS and two 1856 KiB application slots.

Chat already consumed by the UI is RAM-only. Duplicate history is bounded to
32 identities, not permanent replay protection. There is no forward secrecy,
secure boot, or flash encryption. Device access can reveal compiled secrets;
serial diagnostics expose local chat. NVS write wear, power interruption,
runtime heap/stack margins, and packet timing still require physical tests.

One ESP-NOW transmission is outstanding at a time. Channel changes wait for
its completion callback. A callback absent for two seconds enters ERROR rather
than transmitting against uncertain radio state. No delivery is inferred from
MAC send success, neighbor discovery, or synchronization.

## Primary references

- [ESP-NOW API, pinned ESP-IDF generation](https://docs.espressif.com/projects/esp-idf/en/v4.4.8/esp32/api-reference/network/esp_now.html)
- [NVS storage](https://docs.espressif.com/projects/esp-idf/en/v4.4.8/esp32/api-reference/storage/nvs_flash.html)
- [Wi-Fi monitoring and channel configuration](https://docs.espressif.com/projects/esp-idf/en/v4.4.8/esp32/api-guides/wifi.html)
