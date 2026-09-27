# Implementation notes

## Source organization

The bridge follows the [ESPHome external-component layout](https://esphome.io/components/external_components/). All runtime C++ code is inside [`components/halo2/`](../components/halo2/); ESPHome copies it into the generated build automatically.

| File | Responsibility |
|---|---|
| `__init__.py` | Configuration validation, entity creation, and selection of the radio backend |
| `halo2.h`, `halo2.cpp` | One shared lamp state, native entity adapters, command batching, status polling, preference storage, and discovery coordination |
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

Commands arriving before the next 50 ms update are combined into the shared state. The first command is eligible immediately. Following a completed command batch, `command_debounce` imposes a one-second default cooldown; further requests are combined and the latest state is sent when that fixed interval expires:

- `0x03` applies mode, brightness, temperature, and presence settings; it does not change global power.
- `0x02` explicitly changes global power. It takes precedence over settings-only commands within a pending batch.
- When sending ON, the bridge first sends `0x03` with the final settings, then `0x02`. OFF needs `0x02` alone.
- Auto brightness queues `0x03` with control bit 1 set. A subsequent manual brightness or temperature change cancels a pending Auto request.

This ordering makes consecutive front/back commands from HA's group or “all lights” controls work in either arrival order. The diagnostic **Resend current state** button uses the same power-aware sequence.

LR1121 queues the complete one- or two-packet batch before starting it. **Command sent** is published and the cooldown starts only after every packet reports TX_DONE. New HA requests can update the pending state while that batch is in flight. Received frames cannot overwrite pending commands.

Received controller state is published under a guard that prevents the resulting light callbacks from queuing another transmission. Shared-temperature synchronization uses the same guard. The light adapter also tracks whether a deferred write originated locally, so an incoming radio update is not echoed later.

At boot, light preferences and the saved selection initialize the bridge without sending settings or power commands. The saved radio link includes address, channel, and packet options. HA commands are optimistic; received controller requests and validated LR1121 status replies replace this state. Unchanged status replies do not republish the light entities.

## LR1121 scheduling and recovery

The driver advances up to four SPI transactions per ESPHome loop call. Reset pulse timing, startup, BUSY waits, calibration, TX completion, RX reads and rearming all use timed states. Individual SPI transfers remain synchronous and bounded to 32 bytes at 1 MHz; the only explicit delay is one microsecond for NSS-to-BUSY propagation. The component keeps its loop enabled to service the radio independently of the 50 ms application update.

Faults stop the current operation and schedule radio reinitialization with an exponential retry delay of 1–30 seconds. The bridge keeps Wi-Fi/API connectivity and its saved address, channel and packet options. Successful initialization resets the retry delay, restores passive reception or restarts an interrupted discovery, and schedules a fresh lamp-state query. In-flight and pending commands are dropped so recovery cannot replay stale actions. Failed initialization also retries instead of permanently marking the component failed.

## LR1121 status polling

Status polling uses command `0x04`, with a five-second default cycle. It starts after initialization or discovery, and local commands or received controller requests bring the next refresh forward to 500 ms. Polling yields to pending commands and light transitions.

The lamp preloads its ACK payload before processing the triggering request. A single query can therefore return an earlier command's state, including an old ON immediately after HA turns the lamp OFF. Each polling cycle instead:

1. Sends a refresh query and waits for a CRC-valid reply with the matching two-bit packet ID. Its state is discarded, regardless of the command byte.
2. After that acknowledgement, waits 500 ms for the lamp to update its queued payload, then sends a second query.
3. Accepts only a matching reply carrying command `0x04` as current lamp state. If an older command reply is still queued, retries the read after another 500 ms, up to three read attempts in total.

Each reply can time out after 200 ms, measured from TX_DONE rather than from queueing the request; waiting does not block the component loop. A missed refresh acknowledgement prevents subsequent queries from being accepted as fresh. A new local command or received controller request cancels the in-progress cycle. After three failed cycles, the component reports a warning while retaining the last known state; a successful cycle clears it.

The radio switches directly from TX to a 20 ms RX window using `AutoTxRx` (`0x020C`). Returning from TX leaves the captured ACK intact until the next receive-buffer check. Normal RX uses the full 32-bit address sync without a separate preamble gate, so short ACK preambles can be received. After each packet or timeout, the driver disables automatic switching before returning to passive RX; receiving controller traffic must not trigger transmission.

## LR1121 address discovery

The Waveshare board receives raw 2.4 GHz GFSK at 125 kbps, with 160 kHz frequency deviation, Gaussian BT=0.5 shaping, and 467 kHz receive bandwidth. Transmission is configured at 0 dBm. The dedicated 2.4G antenna path is separate from the board's sub-GHz antenna switch; see [antenna routing](WIRING.md#antennas).

Discovery starts if enabled and neither a configured nor saved link is available. It can also be started by the discovery button.

1. Scan channels 5, 46, and 75, alternating `0xAA` / `0x55` preamble-byte sync patterns, with a three-second dwell per combination.
2. Capture 24 bytes after the short sync with the preamble gate disabled. Prepend that sync byte to the capture.
3. Search all bit alignments for a four-byte address followed by a valid complete request.
4. Check framing, CRC, brightness/temperature ranges, and request fields. Count matches by address and channel.
5. After three matching valid captures, configure full 32-bit address sync and normal fixed-width RX, publish the captured state, and save the link to ESPHome preferences.

Only valid controller requests count; arbitrary RF bytes do not identify a link. No particular brightness or temperature is required. Transmission is blocked during discovery.

Normal reception holds one packet until the next receive-buffer check. This prevents an immediate lamp reply from overwriting a captured controller request. RX is rearmed after consuming a packet or completing a receive window; successful transmission leaves automatic ACK reception intact.

## Packet representations

Both backends use a canonical 13-byte representation internally:

```text
PCF (1 byte) | application payload (10 bytes) | CRC16 (2 bytes)
```

The payload is:

| Payload offset | Meaning |
|---|---|
| 0 | Command |
| 1 | Control: power in bit 0, Auto brightness in bit 1, mode in bits 3–4, presence enable in bit 5 |
| 2 | Front brightness, 1–100 |
| 3–4 | Shared temperature in kelvin, big-endian |
| 5 | Back brightness, 1–100 |
| 6–7 | Rear temperature; commands repeat the shared setting here |
| 8 | Packet option observed as `0x00` or `0x01`; preserved from received traffic |
| 9 | Fixed suffix `0x02` |

Modes are front-only `0`, back-only `1`, and both `2`. Request PCFs are formed as `0x50 | ((pid & 3) << 1)`.

PCF bit 0 is No-ACK: controller requests clear it, and lamp replies set it. Discovery and the legacy receiver accept requests only. LR1121 normal reception also decodes replies, but only the polling sequence above may publish their state. Packet ID is a separate two-bit field. Accepted frames must have the expected length, payload-length field, command range, suffix, mode, CRC, and in-range brightness/temperature.

During Auto adjustment, the rear temperature field in replies can lag the front field. The bridge reads the front field as the logical shared temperature, while retaining both independent brightness values.

### LR1121 on-air format

```text
preamble | address (4 bytes, reversed from register order)
         | leading zero PCF bit | canonical PCF byte
         | payload (10 bytes) | CRC16
```

After address synchronization, the radio captures 105 meaningful bits as a 14-byte buffer, with seven padding bits. `make_air_frame()` and `decode_air_frame()` insert/remove the leading PCF bit and pack/unpack this representation.

CRC uses polynomial `0x1021`, initial state `0xFFFF`, and covers the on-air address, the full nine-bit PCF, and payload. The LR1121's hardware CRC is disabled because its packet engine cannot express this CRC coverage and bit alignment. Software still constructs and validates the complete CRC. Whitening is disabled to match the lamp's unwhitened frame format.

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

**Command sent** means the backend completed transmission. On LR1121 this includes observing TX_DONE, not confirming a visible lamp change. **Lamp status received** means a refresh/read polling cycle returned validated lamp state.

The original controller sends settings snapshots, so a later valid request can recover missed updates. LR1121 polling also catches autonomous presence changes, Auto brightness adjustments, and missed requests. Collisions, range, or a disconnected lamp can delay synchronization; polling retains the last known state until a reply arrives. BM5602 remains limited to passive controller synchronization.

The Waveshare implementation has been exercised on hardware for discovery, saved-link restoration, native API control, front/back operation, grouped on/off commands, Auto brightness, and lamp status reception. CI separately compiles both board configurations using the pinned Podman image and runs the existing BM5602 CRC vectors. A successful build or TX_DONE alone does not establish lamp acknowledgement or RF isolation measurements.
