# Initial hardware validation

The ESP32-S3 Wi-Fi RF front end recovered a complete Halo 2 frame without
using the LR1121. Two observations of the same 105-bit raw frame pass an
independent offline CRC check. This establishes basic reception, demodulation,
timing recovery, and packet detection on this board; it does not establish
continuous or reliable reception.

Results below were captured on **2026-10-02 UTC**, using the USB-connected
Waveshare ESP32-S3-LR1121-HF with 4 MB flash and 2 MB quad PSRAM. The firmware
asserts LR1121 reset on GPIO39 and holds its chip select high on GPIO42. There
are no LR1121 SPI commands. Its antenna was disconnected. Reset state is
reported by firmware, rather than measured with an external probe.

The captures used ambient traffic at 2405 MHz, on-air address `CE8C234F`,
125 kbit/s, nominal 16 MS/s input, and a 105-bit observation after the address.
There was no confirmed controller action sequence or simultaneous reference
receiver. The experiment made no transmissions to generate test traffic.

## Capture results

| Run | Firmware source digest | Successful windows | Complete candidates | Offline CRC passes | Firmware capture duty |
|---|---|---:|---:|---:|---:|
| Baseline, 10 s | `5e0e82414406` | 408 | 1 | 1 | 9.41% |
| Baseline, 60 s | `5e0e82414406` | 2,352 | 1 | 0 | 8.97% |
| Optimized detector, 30 s | `82516e0a14de` | 1,616 | 1 | 1 | 12.35% |
| Directly after flashing, 10 s | `82516e0a14de` | 480 | 0 | 0 | 10.99% |

All **4,856 windows** completed without an upstream bank-continuity error or
demodulator overflow. Each window covers approximately 2.3 ms; processing
creates gaps between them. Capture duty is nominal RF time divided by elapsed
firmware batch time, excluding host-side gaps. The differing runs are not a
controlled loss-rate or performance comparison.

The optimized detector uses integer-indexed symbol averages during its initial
address scan, then the same fractional timing recovery and bit tracking. Its
captured frame reports a bit period of **7.992 samples at 1 MS/s** (expected
near 8), minimum bit margin 0.8028, and transition timing error RMS 0.1873
samples. These are DSP estimates, not measurements against a calibrated clock.

## Independent frame validation

| Run | Raw following bits, MSB packed | Received CRC | Calculated CRC | Result |
|---|---|---|---|---|
| Baseline, 10 s | `290218850546050546008142C100` | `8582` | `8582` | Pass |
| Baseline, 60 s | `2A82188505460505460083161F00` | `2C3E` | `ECBB` | Fail |
| Optimized detector, 30 s | `290218850546050546008142C100` | `8582` | `8582` | Pass |

The check uses the established [Halo 2 CRC model](../../docs/PROTOCOL.md#crc):
polynomial `0x1021`, initial `0xFFFF`, MSB first, no reflection or final XOR.
It covers the 32 address bits followed by the first 89 observed bits, and
compares the result with the final 16 observed bits. Seven unused low bits in
the last byte are excluded. No command or settings interpretation is needed.

CRC validation was performed offline and is **not implemented in the
experimental firmware**. A complete observation alone can contain errors, as
the second baseline candidate demonstrates. Without raw I/Q or a paired
reference capture, the mismatch does not locate the error in the receive chain
or establish that the transmitted frame was valid. The two passing observations
contain identical bits; these captures do not cover different settings or
provide a packet-loss estimate.

## Build and startup checks

- All 14 native synthetic-signal cases pass with strict compiler warnings and
  with AddressSanitizer/UndefinedBehaviorSanitizer enabled.
- The standalone ESP-IDF 5.5.5 build and USB flashing pass; esptool verifies the
  flashed data hashes. Startup detects 2 MB PSRAM and passes its memory test.
- Generated DSP sources and the experiment digest match the checked-in sources.
- Python syntax, shell syntax, and Git whitespace checks pass.
- A capture connection initially timed out directly after flashing. The host
  tool now performs the native USB startup reset used by ESP-IDF's monitor.
  The final 10-second run verified connection and capture directly after a
  fresh flash, without opening the monitor first. `--no-reset` skips that reset.

[`validation.json`](validation.json) preserves firmware configuration, source
provenance, complete candidate records, offline CRC results, aggregate capture
counts, and summaries for these four runs. Full local JSONL logs remain in the
ignored `captures/` directory; their SHA-256 hashes are recorded in the report.
The current firmware digest is `82516e0a14de`; the earlier baseline digest
identifies observations made before the detector optimization.
