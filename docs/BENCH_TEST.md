# CYD bench procedure

Use classic 4 MB ESP32-2432S028R boards. Start with two endpoints; add at least
one additional CYD for a real relay experiment. Record firmware commit, board
revision, placement, height, obstacles, channel plan, and delivery times.

1. Build and flash the current checkout using the README. Match A/B to their
   station MACs. Export relay membership with make_relay.py; do not copy the
   endpoint key to a relay. Verify the role, MAC, storage status, and channel in
   the 115200-baud console. First installation uses the shipped partition table.
2. At short distance, send text both ways, including 160 ASCII characters,
   punctuation, and UTF-8. Verify full text and destination receipts. Fill the
   outbox while the other endpoint is powered off; the ninth send must retain
   its composer text. Reboot the sender, then restore the receiver and verify
   queued messages drain. Verify the display/touch alignment and 123/ABC pages.
3. Run one CYD as a relay. Place A and B so they cannot exchange packets
   directly. Confirm the direct failure with the relay powered off, then turn
   the relay on and demonstrate delivery in both directions. A MESH label alone
   is not proof; the recipient's message and sender's + receipt are required.
4. Add a second relay and interrupt the first path. Verify delivery resumes
   through the alternative path. Check /neighbors and /status for discovery,
   rejects, retransmissions, stored packets, and send callback faults.
5. Test moving store-and-forward: let a relay hear A, move it into range of B
   while A is absent, then return it into contact with A for the receipt.
   Repeat with a relay restart after capture. Record the difference between
   delivery time and receipt return time. Keep within forwarding retention.
6. Cut receiver power around message reception and around receipt transmission.
   Confirm persistence and bounded duplicate suppression after reboot. Interrupt
   sender power after SEND. Test a nearly full queue and storage write failures
   with an instrumented build; no failed durable admission may generate a receipt.
7. Observe a channel-plan change. Check current/next masks and activation slot
   on all nodes, deliberately miss the announcement on one board, and verify
   rendezvous recovery. Remove A and confirm stale relayed pulses expire rather
   than sustaining a false clock; relays should fall back to home.
8. Compare ENABLE_CHANNEL_ADAPTATION on/off and ENABLE_BLE_RESCUE on/off using
   identical placements and message workloads. Ordinary authorized Wi-Fi traffic
   can supply a busy-channel condition. Record delivery ratio, latency, retries,
   beacon activity, and range. Do not infer a gain merely from seeing more RF.
9. Test long runs with the full queues and multiple relays. Measure free heap,
   task stack margins, flash write rates, power use, and temperature. Exercise
   shutdown/reboot during NVS writes. No hardware results have been assumed.

These tests distinguish a direct radio link, relayed mesh coverage, and movement
of stored messages. They do not demonstrate ambient backscatter or messages
riding unrelated broadcasts.
