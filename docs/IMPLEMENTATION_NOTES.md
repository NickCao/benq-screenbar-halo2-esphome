# Implementation notes

## Source organization

The bridge follows the [ESPHome external-component layout](https://esphome.io/components/external_components/). All runtime C++ code is inside [`components/halo2/`](../components/halo2/); ESPHome copies it into the generated build automatically.

| File | Responsibility |
|---|---|
| `__init__.py` | Configuration validation, entity creation, SPI registration, and GPIO code generation |
| `halo2.h`, `halo2.cpp` | One shared lamp state, native entity adapters, command batching, status polling, preference storage, and discovery coordination |
| `halo2_protocol.h` | Payloads, CRC, canonical/air-frame conversion, validation, and address extraction; independent of ESPHome and GPIO |
| `lr1121_radio.h` | LR1121 command/packet handling using ESPHome `SPIDevice` and GPIO |

`Halo2` owns the HA-facing state, its LR1121 driver, and the packet sequence counter. The driver uses ESPHome's SPI and GPIO interfaces; protocol encoding remains separate from hardware access.

The [protocol reference](PROTOCOL.md) documents the wire format, capture evidence, confirmed command effects, decoder limits, and open questions. This document describes how the bridge uses that protocol.

## HA state and radio commands

`Halo2` keeps one `HaloRxState`: global power, front/back selection, two brightness values, shared temperature, presence-mode enable, and inactivity timeout. The front/back lights and ultrasonic select are views of this state.

| Front light | Back light | Radio state |
|---|---|---|
| On | On | Power on, both selected |
| On | Off | Power on, front selected |
| Off | On | Power on, back selected |
| Off | Off | Power off, a valid previous selection retained in the frame |

There is no master Power entity. Turning one section off while the other remains on changes selection. Turning off the last section requires global power OFF. Each section's last useful brightness is retained.

Color temperature is one logical setting and is mirrored between the two light entities. The payload contains two temperature fields; the bridge writes the same value to both. Behavior with unequal outgoing temperature fields has not been characterized.

Commands arriving before the next 50 ms update are combined into the shared state. The first command is eligible immediately. Following a completed command batch, `command_debounce` imposes a one-second default cooldown; further requests are combined and the next required command uses the latest state when that fixed interval expires:

- `0x03` applies mode, brightness, temperature, and presence settings; it does not change global power.
- `0x02` explicitly changes global power and is processed before other pending commands.
- When sending ON, the bridge first sends `0x03` with the final settings, then `0x02`, consuming any pending settings request. OFF uses `0x02`; separately requested settings remain pending because power-off does not apply the presence-enable bit.
- `0x05` saves the inactivity timeout. It remains pending through power/settings commands and runs after them, respecting the same cooldown.
- Auto brightness queues `0x03` with control bit 1 set. A subsequent manual brightness or temperature change cancels a pending Auto request.

Pending command types are held in a fixed bitset. Repeated requests of the same type coalesce into the latest shared state, while distinct required operations cannot overwrite one another. A combined power-off, presence-disable, and timeout change therefore sends `0x02`, `0x03`, then `0x05` in separate batches.

The ultrasonic select maps Disabled to a cleared presence bit, preserving the timeout. Its other four options enable presence and select the corresponding duration. Changing enable queues settings; changing duration queues the timeout command. Received state maps both fields back to the same select.

This ordering makes consecutive front/back commands from HA's group or “all lights” controls work in either arrival order. The diagnostic **Resend current state** button uses the same power-aware sequence.

LR1121 queues the complete one- or two-packet batch before starting it. **Command sent** is published and the cooldown starts only after every packet reports TX_DONE. New HA requests can update the pending state while that batch is in flight. Received frames cannot overwrite pending commands.

Received controller state is published under a guard that prevents the resulting light callbacks from queuing another transmission. Shared-temperature synchronization uses the same guard. The light adapter also tracks whether a deferred write originated locally, so an incoming radio update is not echoed later.

At boot, light preferences and the saved selection initialize the bridge without sending settings or power commands. The saved radio link includes address, channel, and timeout; the timeout occupies the former packet-options byte, preserving preference version 2's layout. Local controls require a valid received state after boot or recovery, so UI defaults cannot overwrite the sensor's actual configuration. HA commands are then optimistic; received controller requests and validated LR1121 status replies replace this state. Unchanged status replies do not republish entities.

## LR1121 scheduling and recovery

The driver advances up to four SPI transactions per ESPHome loop call. Reset pulse timing, startup, BUSY waits, calibration, TX completion, RX reads and rearming all use timed states. Individual SPI transfers remain synchronous and bounded to 32 bytes at 1 MHz; the only explicit delay is one microsecond for NSS-to-BUSY propagation. The component keeps its loop enabled to service the radio, checking commands and received frames every 50 ms. ESPHome's `PollingComponent` schedules periodic lamp status queries independently, while named scheduler timeouts handle recovery retries and discovery dwell periods.

The LR1121 driver registers with ESPHome's SPI bus and uses generated GPIO objects for CS, reset, BUSY and IRQ. Each transfer releases CS and the bus before waiting for the radio, allowing other SPI devices to share the bus. Radio recovery reuses the registered device without resetting or freeing the shared bus. ESPHome logs SPI transfer errors; the radio's response checks and BUSY/TX deadlines remain responsible for detecting failed operations because the SPI transfer API has no error return.

Faults stop the current operation and schedule radio reinitialization with an exponential retry delay of 1–30 seconds. The bridge keeps Wi-Fi/API connectivity and its saved address, channel and timeout. Successful initialization resets the retry delay, restores passive reception or restarts an interrupted discovery, and schedules a fresh lamp-state query. In-flight and pending commands are dropped so recovery cannot replay stale actions. Failed initialization also retries instead of permanently marking the component failed.

## LR1121 status polling

Status polling uses command `0x04`, with a five-second default cycle. It starts after initialization or discovery, and local commands or received controller requests bring the next refresh forward to 500 ms. Polling yields to pending commands and light transitions.

Observed ACKs can contain an earlier command's state, consistent with the lamp preloading its reply before processing the triggering request. This includes an old ON immediately after HA turns the lamp OFF. Each polling cycle therefore:

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

The [protocol reference](PROTOCOL.md#framing-and-packet-control-field) is the source for bit order, PCF layout, payload offsets, software CRC, and canonical/air-frame conversion. It includes a reproducible encoding example and explicitly distinguishes known wire behavior from decoder restrictions and bridge timing policies.

## Reliability and verification

**Command sent** means the radio completed transmission and reported TX_DONE, not that a visible lamp change was confirmed. **Lamp status received** means a refresh/read polling cycle returned validated lamp state.

The original controller sends settings snapshots, so a later valid request can recover missed updates. LR1121 polling also catches autonomous presence changes, Auto brightness adjustments, and missed requests. Collisions, range, or a disconnected lamp can delay synchronization; polling retains the last known state until a reply arrives.

The Waveshare implementation has been exercised on hardware for discovery, saved-link restoration, native API control, front/back operation, grouped on/off commands, Auto brightness, lamp status reception, and all ultrasonic select options. Combined power/settings/timeout requests were verified by lamp readback. The [protocol evidence and limitations](PROTOCOL.md#capture-evidence-2026-09-28) distinguish these checks from physical inactivity timing and other untested behavior. CI separately compiles the Waveshare configuration using the pinned Podman image. A successful build or TX_DONE alone does not establish lamp acknowledgement or RF isolation measurements.
