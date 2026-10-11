# ESP32-S3 radio RX/TX experiment

Standalone firmware for the **Waveshare ESP32-S3-LR1121-HF**, based on a pinned
[esp-sdr](https://github.com/ESPARGOS/esp-sdr) receiver and the polar transmit
path from [esp32-sdr-trx](https://github.com/jochenhammes/esp32-sdr-trx).
It uses the ESP32's Wi-Fi RF front end and antenna to receive Halo 2's
125 kbit/s GFSK traffic and transmit compatible 125 kbit/s FSK packets.
Ordinary RX and TX hold the LR1121 in reset (GPIO39 low), with SPI chip select
high (GPIO42). The explicit `--check` test temporarily enables the LR1121 as
a receive-only reference, then restores reset. It never asks the LR1121 to
transmit.

This is USB-controlled firmware, separate from the ESPHome component. Use
the board's **ESP32 Wi-Fi antenna**, beside the FFC connector. The LR1121
antenna connectors are only used by the optional reference reception test.

## What is implemented

1. Capture three contiguous SRAM-bank segments at nominal 16 MS/s using
   esp-sdr's existing bank-rotation and continuity checks.
2. Process the capture on the ESP32 after the RF writer stops: unpack signed
   10-bit I/Q, filter and decimate to 1 MS/s, remove DC, then apply a complex
   phase-difference frequency discriminator.
3. Find the configured 32-bit on-air address in either FSK polarity, estimating
   carrier offset and tone separation from the known bits.
4. Recover bit phase and rate from address transitions, then track transitions
   while collecting a fixed number of following bits.
5. Report raw candidates over USB with timing and signal-quality estimates.
6. Transmit a bounded packet on an explicit USB command: 32 alternating
   preamble bits, the configured address, then 1–256 following bits, MSB first.
7. Optionally compare the transmitted bits with an independent LR1121 capture.

The default frequency is **2405 MHz**, address **CE8C234F in on-air order**, and
observation length **105 bits after the address**. These defaults come from
our previous capture; configure a different address if the lamp's link has
changed. Register-order address `4F238CCE` becomes on-air `CE8C234F`.

RX has no command interpretation or firmware CRC checking. TX sends the
supplied bits; the host tool can construct a ten-byte Halo payload's PCF and
CRC. There is no pairing, HA integration, or automatic state control.
An RX candidate's `complete: true` means all requested bits were collected;
it does **not** mean a valid Halo 2 packet was decoded.

Each capture spans approximately 2.3 ms. Processing and USB output create gaps
between captures. All samples inside a successful window are processed; the
demodulator resets at every window boundary. A packet overlapping a gap can be
missed or truncated. The serial log records nominal captured time and elapsed
time so this duty cycle is visible. This is not a continuous packet sniffer.

The DSP buffer uses the board's 2 MB quad PSRAM. Raw RF capture stays in the
upstream reserved internal SRAM banks, protected by its linker assertions.
No raw I/Q is processed concurrently with the RF writer.

## Build and flash

Run from the repository root:

```sh
python3 experiments/esp32-sdr/prepare.py
experiments/esp32-sdr/idf build
```

`prepare.py` downloads the exact esp-sdr, esp-dsp and esp32-sdr-trx revisions in
[`upstream.json`](upstream.json), retains their licenses, and generates an
ignored `.work/firmware/` tree with a small integration overlay. The `idf`
wrapper regenerates that overlay before each invocation. Generated build files
and captured logs are ignored; edit the sources in this directory instead.

The build wrapper uses an activated ESP-IDF environment when `IDF_PATH` is set.
Otherwise it reuses this repository's existing **ESP-IDF 5.5.5** cache through
the ESPHome 2026.9.0 Podman image. Populate that cache using the main project's
build instructions, or activate a separate ESP-IDF 5.5.5 installation. The
upstream project's SDK pin is a newer 6.2 development revision; this
experiment records that distinction in its provenance instead of claiming
to reproduce upstream's SDK build.

For this board the generated defaults select 4 MB flash, quad PSRAM at 40 MHz,
and the upstream S3 capture configuration. The upstream spectrum-output queue
is reduced from 16 KiB to 4 KiB to make room for this SDK's internal data and
the optional reference receiver's SPI driver.
The RF bank sizes and their ownership checks are unchanged.

Flash the USB-connected experiment board explicitly:

```sh
SDR_SERIAL_DEVICE=/dev/ttyACM0 experiments/esp32-sdr/idf -p /dev/ttyACM0 flash
```

This replaces that board's firmware. Use the normal project firmware to turn
it back into an ESPHome bridge. No firmware backup is created by these tools.

## Capture

Install `requirements.txt` into a Python environment, then run:

```sh
python3 experiments/esp32-sdr/capture.py --port /dev/ttyACM0 --seconds 30
```

On this repository's Podman setup, the existing image supplies Python and
pyserial without a host installation:

```sh
SDR_SERIAL_DEVICE=/dev/ttyACM0 experiments/esp32-sdr/listen --seconds 30
```

The capture tool resets the ESP32 through native USB before connecting, as
ESP-IDF's serial monitor does. This also restores communication after flashing.
Use `--no-reset` to connect to an already running receiver without restarting it.

During capture, adjust brightness on the original controller. A second bridge
on the same lamp link can also supply traffic. Since capture is intermittent,
repeat the activity over several seconds. The running lamp/bridge retains its
usual behavior; the experiment listens only.

`--frequency`, `--sync`, `--bits`, and `--min-power` adjust reception. The power
threshold uses filtered ADC units squared, **not calibrated RSSI or dBm**.
`--windows` controls the number of capture windows per bounded USB command.
The default is 16. Captures are saved as timestamped JSONL in `captures/`.

Each `candidate` record contains:

| Field | Meaning |
|---|---|
| `window`, `batch` | Capture identifiers; sample positions do not span gaps |
| `start_us` | Address start relative to that capture at the nominal sample rate, including DSP filter delay |
| `samples_per_bit` | Recovered bit period at 1 MS/s; expected near 8 |
| `offset_hz`, `deviation_hz` | Frequency estimates from the address bits |
| `sync_error` | RMS tone error divided by estimated deviation; lower is better |
| `power` | Mean filtered power across the address |
| `bit_margin_min` | Smallest following-bit distance from the frequency threshold, divided by deviation; larger is better |
| `timing_error_rms` | RMS following-transition clock correction in samples at 1 MS/s |
| `inverted` | Opposite I/Q/FSK polarity matched the address |
| `bits`, `raw` | Following bits, packed MSB first; unused low bits of the final byte are zero |
| `complete` | Requested observation length reached, without CRC validation |

The `info` record identifies the experiment source digest and runtime settings.
Each `batch` records capture errors, signal peak, sample count, and timing.
`summary` reports measured firmware capture duty, excluding host-side gaps.
No payload command or setting is decoded.

## Transmit

TX adapts esp32-sdr-trx's calibrated PHY test-tone keying and PLL frequency
steering. The exact upstream LO planner, license and notices are kept in
`vendor/esp32-sdr-trx/`; the complete pinned source is downloaded by `prepare.py`.
The hot loop is forced into IRAM, reads a precomputed internal-DRAM schedule,
and changes both lower PLL SDM bytes once per 8 µs bit. It uses the S3 ROM's
SDM transaction format with a bounded busy wait and saves/restores analog-bus
gates. Interrupts are masked only during the short packet.

The implementation is split across these files:

| File | Responsibility |
|---|---|
| [`tx.c`](tx.c), [`tx.h`](tx.h) | Validate packed bits and build absolute PLL words for the preamble, address and padded following bytes |
| [`frame.py`](frame.py) | Construct the nine-bit PCF and calculate the address-dependent CRC |
| [`firmware/halo_tx.inc`](firmware/halo_tx.inc) | Parse TX commands, allocate the schedule, enter PHY TX mode, key the tone, run the bit clock, unkey and prepare RX |
| [`firmware/tx_reference.inc`](firmware/tx_reference.inc) | Configure the optional LR1121 receive-only reference and restore its reset state |
| [`transmit.py`](transmit.py), [`transmit`](transmit) | Configure the USB link, send bounded packets and save transmission evidence |

The tone for logical one is nominally carrier +160 kHz and logical zero is
carrier −160 kHz; inverted mode exchanges them. The imported planner maps
frequency to `LO = 30 MHz × (32 + SDM / 65536)` for the board's 40 MHz crystal.
The packet schedule occupies at most 320 words (1,280 bytes) in internal DRAM.
Both tones must share the top SDM byte, so the hot loop changes only analog
block `0x63` registers 4 and 5. It writes changed bytes in that order.

At 240 MHz CPU frequency, each bit has 1,920 cycles. The loop aborts if an
update starts an entire bit late or the analog-bus busy wait exceeds 2,400
cycles (10 µs). These exits unkey the tone and report `complete: false`;
the caller frees the schedule, exits PHY TX mode, restores SRAM ownership
and prepares RX. A lateness over 120 cycles (0.5 µs) is counted even when
it does not abort the packet. These timing assumptions rely on the pinned
240 MHz, single-core configuration with power management disabled.

The voice stream's 40 kHz update rate is too slow for Halo 2. The research
I/Q playback engine's single buffer holds only 409.6 µs at 40 MS/s, shorter
than a normal frame. On this board, faster PLL updates also exceeded the
write-time budget. The working implementation therefore sends **FSK with
160 kHz tone deviation**; calibrated Gaussian shaping, output power and
spectral compliance have not been established. It successfully turned off
the tested lamp; see [TX validation](TX_VALIDATION.md).

Construct a ten-byte payload's nine-bit PCF and address-dependent CRC:

```sh
# Status request, using this experiment's previously captured snapshot.
SDR_SERIAL_DEVICE=/dev/ttyACM0 experiments/esp32-sdr/transmit \
  --payload 04310A0A8C0A0A8C0102 --pid 1

# Global OFF. Command 02 changes power; this snapshot clears its ON bit.
SDR_SERIAL_DEVICE=/dev/ttyACM0 experiments/esp32-sdr/transmit \
  --payload 02300A0A8C0A0A8C0102 --gain 64 --repeat 3
```

Defaults use 2405 MHz and on-air address `CE8C234F`. Supply `--sync` and
`--frequency` for another link. `--raw HEX --bits N` instead sends already
packed following bits without adding a CRC. Unused low bits must be zero;
the transmitter emits zero padding to finish the final byte. A normal
105-bit frame therefore sends 112 following bits, matching the LR1121 bridge.
`--repeat` is bounded to 1–16 packets with 100 ms gaps.

`--gain` selects an uncalibrated tone gain code, 64 strongest to 127 weakest;
the default is 96. `--inverted` reverses the FSK polarity. The same native USB
reset options and Python dependency as the capture tool apply. Direct Python
use is `python3 experiments/esp32-sdr/transmit.py --port /dev/ttyACM0 ...`.

For an independent reception test, use an address unrelated to your lamp:

```sh
SDR_SERIAL_DEVICE=/dev/ttyACM0 experiments/esp32-sdr/transmit \
  --sync D3916AC5 --payload 03112110682C10680202 --pid 2 --check
```

`--check` releases LR1121 reset, configures only RX, captures the ESP32's
packet, compares the meaningful bits, then tears down SPI and asserts reset.
It works for both FSK polarities. The LR1121's **2.4G** antenna connector is
the reference receiver's RF input; the sub-GHz connector is unused.
This check proves packet transmission to that receiver, not a lamp ACK.

The host saves request bits, firmware settings and each `HALOTXEND` result
in timestamped `captures/*.tx.jsonl`. Its CRC checks are offline. The result
reports completed/expected updates, write duration and lateness in 240 MHz
CPU cycles, RX restoration, and optional received bytes. `elapsed_us` covers
the scheduled waveform through unkeying; it excludes the short carrier lead
and calibration. `reference_raw` normalizes inverted FSK and clears unused
low bits; `reference_raw_on_air` retains the reference's actual FIFO bytes.
`complete` alone proves local loop completion, not delivery to a lamp.

TX accepts 2400–2483 MHz, with a further check that both tones share the
third SDM byte. Unsupported boundaries return `ERR tx_pll_range` before
keying. All three Halo frequencies (2405, 2446, 2475 MHz) passed the reference
test. TX and RX capture are serialized; every completed or aborted TX unkeys
and prepares the ESP32 receiver again.

For a serial terminal, the added commands are:

```text
HALOINFO?
FREQ 2405
BANDWIDTH 13
HALOSYNC CE8C234F
HALOBITS 105
HALOPOWER 64
HALORUN 16
HALOTXGAIN 96
HALOTXPOLARITY 0
HALOTX 105 290218850546050546008142C100
HALOTXCHECK 105 290218850546050546008142C100
```

Wait for `HALOEND` before sending another command. Upstream raw captures remain
available through `RINGCAP 3 6`; its reply is binary and needs esp-sdr's host
tools. Read those complete replies before sending further commands.
For TX, wait for `HALOTXEND` or `ERR`; each command sends exactly one packet.

## Tests and provenance

```sh
experiments/esp32-sdr/test
```

The native C/C++ test generates Gaussian-filtered FSK independently of the
receiver. It checks bit accuracy across sample phases, ±0.4% clock error,
±60 kHz carrier offset, I/Q inversion, additive noise and DC offsets, short
and absent preambles, truncated observations, wrong addresses, noise-only
captures, two packets, arbitrary input chunks, capture gaps, and buffer bounds.
These tests do not establish RF sensitivity, calibrated timing, or loss rate
on hardware.

The TX tests also recover synthesized PLL-word waveforms through the actual
RX DSP across sample phases and both polarities, check packing and buffer
bounds, and compare the host bit codec against independent captured-frame
CRC fixtures. There are 14 RX cases, 10 TX cases and four host codec tests.
The host codec in `frame.py` has no serial or ESP-IDF dependency. Native tests
need a C/C++ compiler and Python 3; USB tools additionally need `requirements.txt`.
With this repository's cached image, run:

```sh
podman run --rm --network none --volume "$PWD:$PWD:z" --workdir "$PWD" \
  --entrypoint bash ghcr.io/esphome/esphome:2026.9.0 experiments/esp32-sdr/test
```

The [initial hardware validation](VALIDATION.md) records two CRC-valid
observations of a real Halo 2 frame, a CRC-failing candidate, and measured
capture duty around 9–12%. CRC checks were performed offline; the firmware
still reports raw candidates only.
The [TX hardware validation](TX_VALIDATION.md) records independent reference
captures and the user-confirmed global OFF command.

`demod.c` / `demod.h` are also the exact sources compiled into the firmware.
`firmware/halo_experiment.inc` supplies RX USB commands and calls the upstream
capture API; `firmware/halo_tx.inc` supplies TX commands and `tx_reference.inc`
the optional receive-only check. `tx.c` / `tx.h` are compiled into both native
tests and firmware. `.work/firmware/experiment-provenance.json` records source revisions,
archive SHA-256 hashes, and the experiment source digest.

Original files in this experiment use the repository's MIT license. The
generated combined firmware includes GPL-3.0-or-later esp-sdr code; its upstream
license and notices are retained in `.work/firmware/`. esp-dsp retains its own
license. The source download/overlay procedure preserves the complete source
needed to rebuild this experiment.
The imported esp32-sdr-trx planner and adapted TX sequence originate under
0BSD; its license and notices are retained separately in `vendor/esp32-sdr-trx/`.
