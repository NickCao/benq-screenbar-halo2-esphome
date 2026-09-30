# Implementation notes

## Source organization

The bridge follows the [ESPHome external-component layout](https://esphome.io/components/external_components/). All runtime C++ code is inside [`components/halo2/`](../components/halo2/); ESPHome copies it into the generated build automatically.

| File | Responsibility |
|---|---|
| `__init__.py` | Configuration validation, entity creation, SPI registration, and GPIO code generation |
| `lamp_state.h` | Lamp settings, section/brightness rules, and requested versus observed state; independent of ESPHome and radio metadata |
| `command_queue.h` | Changed settings to command batches, coalescing, Auto brightness intent, TX completion, and cooldown; independent of ESPHome |
| `bridge_state.h` | Lifecycle and status-poll state machines plus a scoped publication guard; independent of ESPHome |
| `halo2_light.cpp` | ESPHome light conversion, preference restoration, publication, and transition handling |
| `halo2.h`, `halo2.cpp` | Component wiring, event routing, radio I/O, preference storage, and discovery coordination |
| `halo2_protocol.h` | Packed payload/frame structs, CRC, canonical/air-frame conversion, validation, and address extraction; uses ESPHome's CRC, byte-order, and bit-casting helpers |
| `lr1121_radio.h` | LR1121 command/packet handling using ESPHome `SPIDevice` and GPIO |

`Halo2` owns the HA-facing state, its LR1121 driver, and the packet sequence counter. The driver uses ESPHome's SPI and GPIO interfaces; protocol encoding remains separate from hardware access.

The [protocol reference](PROTOCOL.md) documents the wire format, capture evidence, confirmed command effects, decoder limits, and open questions. This document describes how the bridge uses that protocol.

## HA state and radio commands

`LampState` contains global power, a front/back selection enum, two brightness values, shared temperature, presence-mode enable, and inactivity timeout. `LampStateModel` keeps requested settings for outgoing commands and the optimistic UI, plus an optional observation from the last accepted fresh lamp status reply. HA commands and received controller requests update requested settings without changing that observation. Accepted status replies update both, reconciling requests with the lamp. A matching reply records an observation even when it does not require a UI update.

Brightness readback reconciles selected sections. Unselected sections ignore brightness settings on the wire, so their requested brightness is retained for the next ON; the observation still records the lamp's raw stored level. The first baseline after boot or recovery and original-controller snapshots use all received brightness values.

Packet command, PCF, and request/reply direction belong to `ReceivedPacket`, alongside its decoded `LampState`. Decoder success is reported by its return value; initialization belongs to the state model. Packet metadata cannot become part of requested lamp settings.

| Front light | Back light | Radio state |
|---|---|---|
| On | On | Power on, both selected |
| On | Off | Power on, front selected |
| Off | On | Power on, back selected |
| Off | Off | Power off, a valid previous selection retained in the frame |

There is no master Power entity. Turning one section off while the other remains on changes selection. Turning off the last section requires global power OFF. `LampState::set_light()` centralizes these rules and retains each section's last nonzero brightness. The light adapter also restores that brightness to ESPHome after an immediate zero-brightness OFF request, so a following plain ON uses the retained value.

Each light adapter identifies its entity with `Section::FRONT` or `Section::BACK` and owns its ESPHome state pointer and publication guard. It translates framework values into lamp settings and projects received settings back into the entity. The bridge works with those settings rather than reading ESPHome's current and remote values itself.

Hardware output uses ESPHome's `current_values_as_ct()` helper, which applies the light's on/off factor and gamma correction to brightness. Its temperature output is normalized across the declared mired range; the adapter converts it to Kelvin and rounds to the lamp's 25 K steps. Remote brightness is used only to retain the next ON level when effective output is zero.

Light and ultrasonic controls pass their updated settings through `request_state_()`. It gives the before/after settings to `CommandQueue`, then updates requested state and preferences. The queue determines which operations are required; the radio path takes a batch and sends its commands without deciding their meaning or order.

Color temperature is one logical setting and is mirrored between the two light entities. The payload contains two temperature fields; the bridge writes the same value to both. A hardware experiment with unequal `0x03` fields found that visible color followed the first field for both sections and for the rear alone, even though replies retained both values. See the [temperature experiment](PROTOCOL.md#unequal-temperature-experiment-2026-09-29) for the captures and limits of that finding.

Commands arriving before the next 50 ms update are combined into the shared state. The first command is eligible immediately. Following a completed command batch, `command_debounce` imposes a one-second default cooldown; further requests are combined and the next required command uses the latest state when that fixed interval expires:

- `0x03` applies mode, brightness, temperature, and presence settings; it does not change global power.
- `0x02` explicitly changes global power and is processed before other pending commands.
- When sending ON, the queue plans `0x03` with the final settings, then `0x02`, consuming any pending settings request. OFF uses `0x02`; every changed setting remains pending for a later `0x03`, because power-off does not apply those fields.
- `0x05` saves the inactivity timeout. It remains pending through power/settings commands and runs after them, respecting the same cooldown.
- Auto brightness queues `0x03` with control bit 1 set. A subsequent manual brightness or temperature change cancels a pending Auto request.

`CommandQueue` holds pending command types in a fixed bitset and owns its ready, transmitting, and cooldown phases. Taking a batch reserves the transmitter; its cooldown begins at TX_DONE. Repeated requests of the same type coalesce into the latest shared state, while distinct required operations cannot overwrite one another. A combined power-off, presence-disable, and timeout change therefore sends `0x02`, `0x03`, then `0x05` in separate batches. New requests during transmission remain pending for the next batch.

The ultrasonic select maps Disabled to a cleared presence bit, preserving the timeout. Its other four options enable presence and select the corresponding duration. Changing enable queues settings; changing duration queues the timeout command. Received state maps both fields back to the same select.

This ordering makes consecutive front/back commands from HA's group or “all lights” controls work in either arrival order. The diagnostic **Resend current state** button queues power, settings, and timeout, including settings and timeout while the lamp is off.

LR1121 queues the complete one- or two-packet batch before starting it. **Command sent** is published and the cooldown starts only after every packet reports TX_DONE. New HA requests can update the pending state while that batch is in flight. Received frames cannot overwrite pending commands.

Each light adapter publishes received state under its own scoped guard, preventing the resulting callbacks from submitting another request. Shared-temperature synchronization uses the same guarded entity operation. The bridge reads the peer's final values after mirroring and merges them into one request; it does not re-enter the peer's control handler. Nested guards restore their previous value when leaving scope. Publication does not change the lifecycle state. The light adapter also tracks whether a deferred write originated locally, so an incoming radio update is not echoed later.

Polling also checks for divergent ESPHome current and remote values, covering a newly requested fade before ESPHome sets its transformer-active flag on the first loop.

At boot, light preferences and the saved selection restore requested settings without sending settings or power commands. The saved radio link includes address, channel, and timeout; the timeout occupies the former packet-options byte, preserving preference version 2's eight-byte layout. Local controls require a valid received state after boot or recovery, so UI defaults cannot overwrite the sensor's actual configuration. Recovery and starting discovery invalidate the received baseline and observation while retaining requested settings for display. Unchanged status replies do not republish entities.

## LR1121 scheduling and recovery

`BridgeLifecycle` owns command eligibility. Initialization leads to `AWAITING_STATE` for a known link or `DISCOVERING` for a new link. A valid received baseline or completed discovery leads to `ACTIVE`, the only state accepting local commands. Failure enters recovery; retries initialize the radio and require a new baseline. Initialization and recovery each have a discovery variant, so an interrupted scan resumes scanning while a learned link resumes normal reception. Discovery channel changes, listening, and dwell expiry also use explicit phases.

The component loop advances the driver, handles readiness and TX completion, then dispatches discovery or normal reception. Normal processing gives pending commands priority, then receives a packet and services status polling. Command and status-poll state machines identify the owner of each TX_DONE; the component has no separate transmission flag.

The driver advances up to four SPI transactions per ESPHome loop call. Reset pulse timing, startup, BUSY waits, calibration, TX completion, RX reads and rearming all use timed states. Individual SPI transfers remain synchronous and bounded to 32 bytes at 1 MHz; the only explicit delay is one microsecond for NSS-to-BUSY propagation. The component keeps its loop enabled to service the radio, checking commands and received frames every 50 ms. ESPHome's `PollingComponent` schedules periodic lamp status queries independently, while named scheduler timeouts handle recovery retries and discovery dwell periods.

The LR1121 driver registers with ESPHome's SPI bus and uses generated GPIO objects for CS, reset, BUSY and IRQ. Each transfer releases CS and the bus before waiting for the radio, allowing other SPI devices to share the bus. Radio recovery reuses the registered device without resetting or freeing the shared bus. ESPHome logs SPI transfer errors; the radio's response checks and BUSY/TX deadlines remain responsible for detecting failed operations because the SPI transfer API has no error return.

Faults stop the current operation and schedule radio reinitialization with an exponential retry delay of 1–30 seconds. The bridge keeps Wi-Fi/API connectivity and its saved address, channel and timeout. Successful initialization resets the retry delay, restores passive reception or restarts an interrupted discovery, and schedules a fresh lamp-state query. In-flight and pending commands are dropped so recovery cannot replay stale actions. Failed initialization also retries instead of permanently marking the component failed.

## LR1121 status polling

`StatusPoll` owns the refresh/read cycle with explicit delay, transmission, and reply-wait phases. It handles PID matching, read retry limits, wrap-safe deadlines, cancellation, and consecutive failure counts. Only its `STATE` result permits applying a lamp observation; the component handles radio I/O and defers new requests while commands or light transitions are pending.

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

The opt-in [pytest hardware suite](../tests/hardware/) sends native API commands and checks fresh lamp readback in DEBUG logs. It covers independent brightness, color-temperature conversion at both limits and an interior value, shared temperature during a peer fade, grouped on/off in both arrival orders, section selection, zero-brightness OFF followed by plain ON, combined power/presence/timeout changes, full resend while off, and all ultrasonic timeout options. It also checks HA entity values for retained brightness and shared temperature. These tests operate the lamp and require an explicit `--halo2-device`; ordinary test runs skip them.

The original controller sends settings snapshots, so a later valid request can recover missed updates. LR1121 polling also catches autonomous presence changes, Auto brightness adjustments, and missed requests. Collisions, range, or a disconnected lamp can delay synchronization; polling retains the last known state until a reply arrives.

The Waveshare implementation has been exercised on hardware for discovery, saved-link restoration, native API control, front/back operation, grouped on/off commands, Auto brightness, lamp status reception, and all ultrasonic select options. Combined power/settings/timeout requests were verified by lamp readback. The [protocol evidence and limitations](PROTOCOL.md#capture-evidence-2026-09-28) distinguish these checks from physical inactivity timing and other untested behavior. Native tests cover the lamp state model's grouped commands, brightness retention, request/observation separation, readback reconciliation, and recovery; command batches, coalescing, in-flight requests, Auto cancellation, resend, cooldown, and reset; and lifecycle gating, discovery recovery, nested publication, polling freshness, retries, cancellation, and clock wraparound. CI runs those tests and compiles the Waveshare configuration using the pinned Podman image. A successful build or TX_DONE alone does not establish lamp acknowledgement or RF isolation measurements.
