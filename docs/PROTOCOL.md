# GHOSTLINK protocol v1

GHOSTLINK is a two-node, infrastructure-free messenger for ESP32 CYDs. It is deliberately closed to the two configured peers: it does not scan for arbitrary devices, propagate firmware, join third-party networks, or forward strangers' traffic.

## Radio planes

- **ESP-NOW:** encrypted unicast data plane.
- **BLE:** low-duty-cycle rendezvous helper only. Node A advertises a signed timing hint. Node B scans only after it has already lost the ESP-NOW schedule.

## Roles

- **A / anchor:** owns the hop clock and continuously emits encrypted `SYNC` frames while following the pair's deterministic channel schedule.
- **B / hunter:** follows A. On timeout it walks channels 1..11 until it catches A again. A valid BLE hint can accelerate this recovery.

## Secret-derived hopping

For slot `S`, both peers calculate:

`channel = 1 + HMAC-SHA256(HOP_KEY, NETWORK_ID || S)[0] mod 11`

The hop interval defaults to 1200 ms. The sequence is pair-specific and deterministic.

Channel hopping is for resilience and autonomous rendezvous, not a substitute for encryption.

## ESP-NOW security

Each peer is provisioned with the same pair-specific ESP-NOW PMK and LMK. Only the configured peer MAC is registered. Incoming application frames are also rejected unless their source MAC is the configured peer and the packet magic/version/role are valid.

## Packets

All packet types share a fixed-size structure under ESP-NOW's v1 payload limit:

- `SYNC`: anchor slot + position inside the slot
- `MESSAGE`: sequence number + chat text
- `ACK`: application-layer acknowledgement
- `PING`: keeps link state/RSSI fresh

The sender holds one message in-flight, retries across hops, and marks it delivered only after an application ACK.

## Lost-link recovery

1. B sees no peer frames for `PEER_LOST_MS`.
2. B enters `HUNT` and scans 2.4 GHz channels 1..11.
3. A continues hopping and repeatedly emits encrypted `SYNC` packets.
4. When B catches a valid `SYNC`, it seeds its slot clock and resumes the pair hop sequence.
5. Optionally, BLE advertises a signed slot hint that B can validate before jumping directly near the current Wi-Fi slot.

## BLE hint

The BLE manufacturer payload contains:

- magic `GL`
- pair `NETWORK_ID`
- slot
- milliseconds into slot
- first four bytes of HMAC-SHA256 over those values with `HOP_KEY`

The hint does not carry chat content or ESP-NOW keys.

## Future protocol work

- two-page symbol/punctuation keyboard
- flash-backed message queue
- key rotation / re-pair UI
- optional forward secrecy at the application layer
- duplicate window larger than one packet
- adaptive hop dwell based on link quality
- BLE rescue hardening and coexistence tuning
- accessibility themes and larger keys

## v0.2 PHY and rendezvous behavior

GHOSTLINK v0.2 can run ESP-NOW over Espressif `WIFI_PROTOCOL_LR`. Both paired
nodes must use the same PHY profile. Peer channel `0` is retained so ESP-NOW
follows the local radio channel as the secret-derived scheduler hops.

When Node B loses synchronization it enters a bounded 1..11 channel sweep. At
the same time, BLE rescue scans for a signed manufacturer-data hint emitted by
Node A. The hint contains protocol version, role, network ID, slot and
milliseconds-into-slot plus a truncated HMAC derived from the pair's hopping
key. A valid hint lets B reconstruct the current Wi-Fi slot immediately and
resume ESP-NOW hunting near the anchor's actual position.

BLE is a control-plane rescue mechanism only. It carries no user messages or
ESP-NOW encryption keys.
