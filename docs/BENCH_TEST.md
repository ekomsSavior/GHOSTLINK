# Bench test: two CYDs

Build verification has passed; radio and touchscreen behavior still need two
physical boards. Record the firmware version, board revision and test results.

1. Flash the setup firmware to each board and record its station MAC. Verify
   display orientation and touch alignment, including corners and 123/ABC.
2. Generate one private pair configuration with the two actual MACs. Flash A
   to the matching A board and B to B. Both must run protocol v3.
3. Place them 1-2 metres apart. Confirm B goes HUNT to LINK; A shows LINK after
   B's ping/message. Use /status on each serial console.
4. Send 20 numbered messages each way, then type on both at once. Every +
   must correspond to one displayed peer message.
5. Power off B. Queue eight messages on A. Verify Q:8; a ninth SEND retains
   its composer. Power B on. Verify automatic discovery and ordered delivery.
6. Repeat with A powered off and messages queued on B. Also restart only A
   while B remains powered; new session/sequence values must not be mistaken
   for old duplicates.
7. Restart the sending board. Its RAM outbox should clear as documented.
   Restart the receiver during traffic and check for the documented possible
   duplicate when an ACK was lost just before reboot.
8. Build both with ENABLE_BLE_RESCUE=false. Repeat recovery, including a
   disconnect longer than nine seconds. Verify B waits on the home channel
   and rejoins a later rendezvous slot. Expect slower recovery under loss.
9. Restore BLE, repeat, and compare time to first authenticated Wi-Fi packet.
   A BLE observation alone must not make LINK appear.
10. Walk apart gradually outdoors with clear sight. Log distance, obstructions,
    delivery latency, txfail counts and recovery time. Turn back toward the
    peer after losing the link. Do not infer range from a successful compile.

Use /status before and after each outage. The pinned Arduino 2.x callback
cannot report receive RSSI, so compare delivery/counter measurements instead.
No range claim is validated until this test has been performed.
