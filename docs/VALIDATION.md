# Validation on 2026-10-15

Target: classic ESP32-2432S028R CYD, PlatformIO 6.2.0,
espressif32 7.1.3, Arduino ESP32 2.0.17, NimBLE-Arduino 2.3.6.

| Check | Result |
| --- | --- |
| ghost-a setup build | PASS |
| ghost-b setup build | PASS |
| ghost-a synthetic paired build | PASS |
| ghost-b synthetic paired build | PASS |
| Host protocol suite | PASS |
| Address/undefined-behavior sanitizers on protocol tests | PASS |
| Pair configuration validation and fresh-key generation | PASS |
| Git whitespace check | PASS |

The protocol suite exercises unsigned timer wrap, delayed sync processing,
nonoverlapping data windows, malformed packet rejection, frame replay,
sender reboot identities, stale/out-of-order ACKs, full receive/outbound
queues, duplicate suppression and 400 deliveries with simulated data/ACK loss,
receiver backpressure and temporary total outages in both directions.

These are component tests and firmware compilation, not an RF simulation or
hardware validation. No serial ESP32 device was connected during this work.
Range, over-the-air recovery, coexistence, display orientation and touch
calibration remain to be checked using BENCH_TEST.md.

Synthetic MACs were used only to compile the paired path. The generated
private test header was removed afterwards, and both final local firmware
images were rebuilt in unpaired SETUP mode. No usable pair keys are published.

The paired builds occupy about 80.5% of the default application flash
partition and 17.3% of static RAM. Setup builds occupy about 71.2% of flash
and 15.7% of static RAM. Runtime heap/stack margins require measurement on
hardware. TFT_eSPI's warning about its optional built-in touch functions is
expected: this project uses XPT2046_Touchscreen on a separate SPI peripheral.
