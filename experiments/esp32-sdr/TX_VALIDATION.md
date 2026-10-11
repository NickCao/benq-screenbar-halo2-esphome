# ESP32-S3 TX hardware validation

The ESP32 Wi-Fi RF front end transmitted a global OFF command to the paired
Halo 2 on **2026-10-11 UTC**. The user confirmed: “Ok, I see the light off”.
The LR1121 remained in reset throughout those transmissions. This establishes
control of the tested lamp through the new ESP32 TX path.

The board is the USB-connected Waveshare ESP32-S3-LR1121-HF, 4 MB flash and
2 MB quad PSRAM, USB identity `7C:4F:AD:0A:8B:EC`. The tested firmware digest
is `3680edbada63`. Build sources, archive hashes, firmware binary hash, full
packet reports and local log hashes are preserved in
[`tx-validation.json`](tx-validation.json).

## Independent reception

The explicit `HALOTXCHECK` diagnostic temporarily configures the onboard
LR1121 as a **receive-only** reference with automatic transmission disabled.
It receives packets emitted by the ESP32, compares meaningful bits, tears
down SPI and restores reset. The ESP32 supplies all transmissions. These
checks use test address `D3916AC5`, unrelated to the lamp's address.

| Frequency | Polarity | Following bits | Packets | Exact matches | Offline CRC passes |
|---|---|---:|---:|---:|---:|
| 2405 MHz | Normal | 105 | 8 | 8 | 8 |
| 2405 MHz | Inverted | 105 | 3 | 3 | 3 |
| 2446 MHz | Normal | 105 | 3 | 3 | 3 |
| 2475 MHz | Normal | 105 | 3 | 3 | 3 |
| 2405 MHz | Normal | 256 | 1 | 1 | Not a Halo frame |
| 2405 MHz | Normal | 1 | 1 | 1 | Not a Halo frame |

All **19 packets** matched. The 17 normal Halo-length packets passed the
offline, address-dependent `0x1021` CRC. The maximum-length packet contains
32 bytes of `A5 5A`; the one-bit packet checks final-byte padding. A previous
prototype (`9c60775978d2`) also recovered one CRC-valid frame and is recorded
separately from the current-firmware totals.

There were no update starts more than 0.5 µs late. The maximum measured start
lateness was 13 cycles at 240 MHz (about 0.054 µs); the maximum measured write
slice was 1,566 cycles (6.525 µs), inside the 1,920-cycle bit interval.
These are firmware timing estimates, not a calibrated RF clock or spectrum
measurement. The 105-bit observation sends 112 following bits including
zero padding, for a 1,408 µs nominal waveform plus the short carrier lead
and unkeying overhead.

## Lamp OFF test

The host generated the nine-bit PCF and CRC from payload
`02 30 0A 0A 8C 0A 0A 8C 01 02`, PID 0 and on-air address `CE8C234F`.
Command `02` changes global power; control bit 0 is cleared.

```sh
SDR_SERIAL_DEVICE=/dev/ttyACM0 experiments/esp32-sdr/transmit \
  --payload 02300A0A8C0A0A8C0102 --pid 0 --gain 64 --repeat 3
```

Three packets were sent at 2405 MHz, 100 ms apart. Raw following bits were
`28011805054605054600815ED680`, with CRC `BDAD`. Each transmission completed
176 updates with no late update, restored RX, and kept the LR1121 in reset.
The user observed the lamp turn off after this run. No lamp ACK was captured;
an ensuing eight-second intermittent ESP32 RX capture found no candidates.
The observation is physical user confirmation rather than a status readback.

## Implementation and checks

TX adapts the PHY test-tone keying and PLL modulation in
[esp32-sdr-trx](https://github.com/jochenhammes/esp32-sdr-trx/blob/6de35a5138c8f6d7bf6af2b6c8dd0c99342e72f0/firmware/src/radio.c),
using its exact 0BSD LO planner. The pinned commit is from `research/iq-tx`.
The complete upstream source remains available through the download/overlay
procedure; imported license and notices are retained.

The Halo path updates both lower SDM bytes at 125 kHz. A forced IRAM loop
and an internal-DRAM schedule avoid flash and PSRAM cache misses during a
packet. The hot loop uses the S3 ROM's analog-bus transaction format,
identified from the cached Espressif ROM ELF, with a bounded busy wait;
analog-bus gate settings and RF bank ownership are restored afterward.

Prototype 500 kHz and 250 kHz shaped-waveform loops exceeded their timing
budgets and aborted. The working path emits **FSK**, one tone per bit at
nominal ±160 kHz, without claiming calibrated Gaussian shaping. The upstream
I/Q engine's 409.6 µs buffer at 40 MS/s is too short for a complete Halo frame;
its documented live-refill limitations are why this implementation uses the
PLL path. Prototype logs and the first successful run are retained in the
JSON report to distinguish them from the completed checks.

The native suite passes 14 RX cases, 10 TX cases and four host codec tests.
Both C/C++ suites also pass AddressSanitizer and UndefinedBehaviorSanitizer.
The ESP-IDF 5.5.5 build, USB flashing and esptool hash verification pass.
Original SRAM-bank ownership and linker assertions remain intact; the unused
spectrum output queue is 4 KiB to leave room for the SPI driver.

Six malformed TX commands (length, hex, padding and trailing arguments) were
rejected. The SDM third-byte boundary at 2430 MHz returned
`ERR tx_pll_range` before keying. Four subsequent RX windows completed without
continuity or overflow errors. Final USB settings were restored to 2405 MHz,
address `CE8C234F`, 105 following bits, normal TX polarity and gain code 96,
with the LR1121 held in reset.

These results establish packet transmission and one OFF action on one board
and lamp. They do not measure output power, sensitivity, range, packet-loss
rate, harmonics or other unwanted emissions, and they do not establish a
complete lamp-command test matrix. RX remains intermittent as described in
the [original RX validation](VALIDATION.md).
