# Implementation notes

## Source organization

The bridge follows the [ESPHome external-component layout](https://esphome.io/components/external_components/). All runtime C++ code is inside [`components/halo2/`](../components/halo2/); ESPHome copies it into the generated build automatically.

| File | Responsibility |
|---|---|
| `__init__.py` | Configuration validation, entity creation, and selection of the radio backend |
| `halo2.h`, `halo2.cpp` | One shared lamp state, native light/switch adapters, command batching, preference storage, and discovery coordination |
| `halo2_protocol.h` | Payloads, CRC, canonical/air-frame conversion, validation, and address extraction; independent of ESPHome and GPIO |
| `lr1121_radio.h` | LR1121 command/packet handling over a templated transport |
| `lr1121_halo2.h` | Waveshare GPIO/SPI transport and LR1121 adapter |
| `bm5602_halo2.h` | Legacy ATOM Lite/BM5602 SPI, clocked direct TX, and passive RX |

Python code generation defines `USE_HALO2_LR1121` or `USE_HALO2_BM5602`. Hardware-specific headers are guarded by these defines because ESPHome includes component headers in its generated umbrella header. This prevents the unselected board's GPIO code from entering the build.

The radio backends retain their existing header-based implementation. The LR1121 command engine is templated; the timing-sensitive legacy BM5602 routines remain together. The component owns the HA-facing state, and protocol encoding is shared between backends.

## HA state and radio commands

`Halo2` keeps one `HaloRxState`: global power, front/back selection, two brightness values, shared temperature, presence-mode enable, and packet options. The front and back light entities are views of this state.

| Front light | Back light | Radio state |
|---|---|---|
| On | On | Power on, both selected |
| On | Off | Power on, front selected |
| Off | On | Power on, back selected |
| Off | Off | Power off, a valid previous selection retained in the frame |

There is no master Power entity. Turning one section off while the other remains on changes selection. Turning off the last section requires global power OFF. Each section's last useful brightness is retained.

Color temperature is one logical setting and is mirrored between the two light entities. The payload contains two temperature fields; the bridge writes the same value to both, matching the lamp's documented shared-temperature behavior.

Commands arriving before the next 50 ms update are combined into the shared state:

- `0x03` applies mode, brightness, temperature, and presence settings; it does not change global power.
- `0x02` explicitly changes global power. It takes precedence over settings-only commands within a pending batch.
- When sending ON, the bridge first sends `0x03` with the final settings, then `0x02`. OFF needs `0x02` alone.

This ordering makes consecutive front/back commands from HA's group or “all lights” controls work in either arrival order. The diagnostic **Resend current state** button uses the same power-aware sequence.

Received controller state is published under a guard that prevents the resulting light callbacks from queuing another transmission. Shared-temperature synchronization uses the same guard. The light adapter also tracks whether a deferred write originated locally, so an incoming radio update is not echoed later.

At boot, light preferences and the saved selection initialize the bridge without transmitting. The saved radio link includes address, channel, and packet options. HA commands are optimistic; received controller requests replace this state when heard.

## LR1121 address discovery

The Waveshare board receives raw 2.4 GHz GFSK at 125 kbps, with 160 kHz frequency deviation, Gaussian BT=0.5 shaping, and 467 kHz receive bandwidth. Transmission is configured at 0 dBm. The dedicated 2.4G antenna path is separate from the board's sub-GHz antenna switch; see [antenna routing](WIRING.md#antennas).

Discovery starts if enabled and neither a configured nor saved link is available. It can also be started by the discovery button.

1. Scan channels 5, 46, and 75, alternating `0xAA` / `0x55` preamble-byte sync patterns, with a three-second dwell per combination.
2. Disable the normal preamble gate and capture 24 bytes after the short sync. Prepend that sync byte to the capture.
3. Search all bit alignments for a four-byte address followed by a valid complete request.
4. Check framing, CRC, brightness/temperature ranges, and request fields. Count matches by address and channel.
5. After three matching valid captures, configure full 32-bit address sync and normal fixed-width RX, publish the captured state, and save the link to ESPHome preferences.

Only valid controller requests count; arbitrary RF bytes do not identify a link. No particular brightness or temperature is required. Transmission is blocked during discovery.

Normal reception holds one packet until the next poll. This prevents an immediate lamp reply from overwriting a captured controller request. RX is rearmed after polling and after transmission attempts.

## Packet representations

Both backends use a canonical 13-byte representation internally:

```text
PCF (1 byte) | application payload (10 bytes) | CRC16 (2 bytes)
```

The payload is:

| Payload offset | Meaning |
|---|---|
| 0 | Command |
| 1 | Control: power in bit 0, mode in bits 3–4, presence enable in bit 5 |
| 2 | Front brightness, 1–100 |
| 3–4 | Shared temperature in kelvin, big-endian |
| 5 | Back brightness, 1–100 |
| 6–7 | Same temperature, repeated |
| 8 | Packet option observed as `0x00` or `0x01`; preserved from received traffic |
| 9 | Fixed suffix `0x02` |

Modes are front-only `0`, back-only `1`, and both `2`. Request PCFs are formed as `0x50 | ((pid & 3) << 1)`.

For synchronization, the decoder rejects frames with PCF bit 0 set: the observed lamp replies carry response metadata rather than reliable requested state. This is a distinction in the low PCF bit, not the parity of the two-bit packet ID. Accepted frames must also have the expected length, payload-length field, command range, suffix, mode, CRC, and in-range brightness/temperature.

### LR1121 on-air format

```text
preamble | address (4 bytes, reversed from register order)
         | leading zero PCF bit | canonical PCF byte
         | payload (10 bytes) | CRC16
```

After address synchronization, the radio captures 105 meaningful bits as a 14-byte buffer, with seven padding bits. `make_air_frame()` and `decode_air_frame()` insert/remove the leading PCF bit and pack/unpack this representation.

CRC uses polynomial `0x1021`, initial state `0xFFFF`, and covers the on-air address, the full nine-bit PCF, and payload. The LR1121's hardware CRC and whitening are disabled; software constructs and validates the complete frame.

See the [Semtech LR1121 user manual](https://files.waveshare.com/wiki/Core1121/UserManual_LR1121_v1_2.pdf) for the transceiver commands and RF configuration. The ScreenBar framing above is implemented by this project, not provided by a LoRa modem.

### Legacy BM5602 format

The BM5602 backend retains its previously working byte-aligned FIFO/direct-input representation. Its CRC model starts at `0xEFDF` and covers the reversed address, canonical PCF byte, and payload. Do not apply these byte-aligned vectors to LR1121's raw nine-bit air format.

During direct transmission, GIO2 becomes data input and GIO3 exposes TBCLK. The ESP32 changes each bit on the low TBCLK edge, sends MSB first, and then returns the BM5602 to passive receive mode. A missing clock produces `BM5602 NO CLOCK`.

The existing [`test_bm5602_crc.py`](../tests/test_bm5602_crc.py) vectors use register address `9C EA BB 86`:

| Vector | PCF | CRC |
|---|---|---|
| Stock request | `0x54` | `0x20B9` |
| Power on | `0x50` | `0xE962` |
| Power off | `0x50` | `0x0241` |

These are legacy CRC reference checks, not an end-to-end test of the component or LR1121 driver.

## Reliability and verification

The bridge does not poll the lamp for status and does not use lamp replies as acknowledgements. **Command sent** means the backend completed transmission. On LR1121 this includes observing TX_DONE, not confirming a visible lamp change.

The original controller sends settings snapshots, so a later valid request can recover missed updates. Reception is still best effort: collisions, range, simultaneous bridge transmission, autonomous presence changes, and power interruptions can leave HA state stale.

The Waveshare implementation has been exercised on hardware for discovery, saved-link restoration, native API control, front/back operation, and grouped on/off commands. CI separately compiles both board configurations using the pinned Podman image and runs the existing BM5602 CRC vectors. A successful build or TX_DONE does not establish lamp acknowledgement or RF isolation measurements.
