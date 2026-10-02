# ESP32-S3 radio reception experiment

Standalone firmware for the **Waveshare ESP32-S3-LR1121-HF**, based on a pinned
[esp-sdr](https://github.com/ESPARGOS/esp-sdr) receiver. It uses the ESP32's
Wi-Fi RF front end and antenna to look for the Halo 2's 125 kbit/s GFSK signal.
The LR1121 is held in reset (GPIO39 low), with its SPI chip select held high
(GPIO42). This firmware sends no commands to the LR1121.

This is a USB-controlled receiver, separate from the ESPHome component. Use
the board's **ESP32 Wi-Fi antenna**, beside the FFC connector. The LR1121
antenna connectors are not used by this experiment.

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

The default frequency is **2405 MHz**, address **CE8C234F in on-air order**, and
observation length **105 bits after the address**. These defaults come from
our previous capture; configure a different address if the lamp's link has
changed. Register-order address `4F238CCE` becomes on-air `CE8C234F`.

There is no command interpretation, CRC checking, pairing, transmission, HA
integration, or automatic state control. `complete: true` means all requested
bits were collected; it does **not** mean a valid Halo 2 packet was decoded.

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

`prepare.py` downloads the exact esp-sdr and esp-dsp revisions in
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
is reduced from 16 KiB to 8 KiB to make room for this SDK's internal data.
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

For a serial terminal, the added commands are:

```text
HALOINFO?
FREQ 2405
BANDWIDTH 13
HALOSYNC CE8C234F
HALOBITS 105
HALOPOWER 64
HALORUN 16
```

Wait for `HALOEND` before sending another command. Upstream raw captures remain
available through `RINGCAP 3 6`; its reply is binary and needs esp-sdr's host
tools. Read those complete replies before sending further commands.

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

The [initial hardware validation](VALIDATION.md) records two CRC-valid
observations of a real Halo 2 frame, a CRC-failing candidate, and measured
capture duty around 9–12%. CRC checks were performed offline; the firmware
still reports raw candidates only.

`demod.c` / `demod.h` are also the exact sources compiled into the firmware.
`firmware/halo_experiment.inc` supplies the USB commands and calls the upstream
capture API. `.work/firmware/experiment-provenance.json` records source revisions,
archive SHA-256 hashes, and the experiment source digest.

Original files in this experiment use the repository's MIT license. The
generated combined firmware includes GPL-3.0-or-later esp-sdr code; its upstream
license and notices are retained in `.work/firmware/`. esp-dsp retains its own
license. The source download/overlay procedure preserves the complete source
needed to rebuild this experiment.
