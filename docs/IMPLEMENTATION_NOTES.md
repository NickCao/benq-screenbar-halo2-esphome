# Implementation notes

## Source organization

The bridge follows the [ESPHome external-component layout](https://esphome.io/components/external_components/). All runtime C++ code is inside [`components/halo2/`](../components/halo2/); ESPHome copies it into the generated build automatically.

| File | Responsibility |
|---|---|
| `__init__.py` | Configuration validation, entity creation, SPI registration, and GPIO code generation |
| `lamp_state.h` | Lamp settings and section/brightness rules; independent of ESPHome and radio metadata |
| `command_queue.h` | Changed settings to command batches, coalescing, Auto brightness intent, TX completion, and cooldown; independent of ESPHome |
| `bridge_state.h` | Lifecycle and status-poll state machines plus a scoped publication guard; independent of ESPHome |
| `halo2_light.cpp` | Master light conversion, section settings, proportional dimming, preference restoration, and readback |
| `halo2.h`, `halo2.cpp` | Component wiring, event routing, radio I/O, preference storage, and discovery coordination |
| `halo2_protocol.h` | Packed payload/frame structs, CRC, canonical/air-frame conversion, validation, and address extraction; uses ESPHome's CRC, byte-order, and bit-casting helpers |
| `lr1121_radio.h` | LR1121 command/packet handling using ESPHome `SPIDevice` and GPIO |

`Halo2` owns the HA-facing state, its LR1121 driver, and the packet sequence counter. The driver uses ESPHome's SPI and GPIO interfaces; protocol encoding remains separate from hardware access.

The [protocol reference](PROTOCOL.md) documents the wire format, capture evidence, confirmed command effects, decoder limits, and open questions. This document describes how the bridge uses that protocol.

The [independent protocol probes](PROTOCOL_EXPERIMENTS.md) additionally establish `0x06` as an Auto action and `0x0B` as a presence-enable command on the tested pair. Production decoding still supports `0x00..0x05`, `0x07`, and `0x08`; the experiment did not extend the runtime command set or relax validation for other lamp-accepted values.

## HA state and radio commands

`LampState` contains global power, a front/back selection enum, two brightness values, shared temperature, presence-mode enable, and inactivity timeout. `Halo2` stores these settings directly for outgoing commands and the optimistic UI. HA commands update them; controller snapshots and accepted fresh lamp status replies replace them through the same `apply_received_()` path. The coordinator handles packet direction and reply freshness. `BridgeLifecycle::active()` indicates whether a received baseline has been established. The first valid snapshot publishes even when its settings match the displayed state; matching snapshots do not republish an active baseline.

Fresh brightness readback reconciles both stored levels, including unselected sections. Controller snapshots likewise replace the complete settings. Number entities show the actual stored values independently of selection and power. The lamp applies brightness only to selected sections, so inactive number edits return to the stored level; select a section before editing it.

Packet command, PCF, and request/reply direction belong to `ReceivedPacket`, alongside its decoded `LampState`. Decoder success is reported by its return value; baseline validity belongs to the lifecycle. Packet metadata cannot become part of requested lamp settings.

Original-controller Favorite recall (`0x07`) and save (`0x08`) requests use the same snapshot path as other controller requests: publish settings without echoing a command, then schedule fresh lamp polling to reconcile the actual state. Their command codes remain packet metadata, and the Favorite flag is not retained. The bridge has no favorite storage or save/recall actions and does not transmit these commands. Favorite replies can acknowledge a matching refresh query, but only a fresh matching `0x04` reply can publish lamp state.

| Entity | Lamp setting |
|---|---|
| ScreenBar power | Global power |
| ScreenBar color temperature | Shared temperature, written to both payload fields |
| Lighting mode | Front / Back / Both selection enum |
| Front brightness / Back brightness | Stored 1–100% brightness fields, including inactive sections |

One `Halo2Light` owns the master ESPHome state pointer and publication guard. Its brightness is the maximum of the selected levels. Master adjustments scale those selected levels proportionally, using integer percentages with a 1% minimum; inactive levels stay unchanged. A fade uses the profile from the last master publication as a fixed reference, so rounding each sample cannot progressively change the ratio. Controller snapshots and fresh lamp replies publish a new reference. There is no cross-light temperature or power synchronization.

Global OFF retains selection and both stored brightness fields. OFF transitions defer the power command until completion and never convert fade samples into stored brightness settings. The adapter restores a nonzero master brightness after a zero-brightness OFF request so a following plain ON restores the retained profile. Number and selection controls keep global power unchanged and finish an active master transition at its target before applying their setting.

Hardware output uses ESPHome's `current_values_as_ct()` helper, which applies the light's on/off factor and gamma correction to effective brightness. Its temperature output is normalized across the declared mired range; the adapter converts it to Kelvin and rounds to the lamp's 25 K steps. A positive master output below half a percent is clamped to the lamp's 1% minimum.

Master light, section settings, and ultrasonic controls pass updated settings through `request_state_()`. It gives before/after settings to `CommandQueue`, then updates requested state and preferences. The queue determines which operations are required; the radio path takes a batch and sends its commands without deciding their meaning or order.

Color temperature is one logical setting owned by the master light. The payload contains two temperature fields; the bridge writes the same value to both. A hardware experiment with unequal `0x03` fields found that visible color followed the first field for both sections and for the rear alone, even though replies retained both values. See the [temperature experiment](PROTOCOL.md#unequal-temperature-experiment-2026-09-29) for the captures and limits of that finding.

Commands arriving before the next 50 ms update are combined into the shared state. The first command is eligible immediately. Following a completed command batch, `command_debounce` imposes a one-second default cooldown; further requests are combined and the next required command uses the latest state when that fixed interval expires:

- `0x03` applies mode, brightness, temperature, and presence settings; it does not change global power.
- `0x02` explicitly changes global power and is processed before other pending commands.
- When sending ON, the queue plans `0x03` with the final settings, then `0x02`, consuming any pending settings request. OFF uses `0x02`; every changed setting remains pending for a later `0x03`, because power-off does not apply those fields.
- `0x05` saves the inactivity timeout. It remains pending through power/settings commands and runs after them, respecting the same cooldown.
- Auto brightness queues `0x03` with control bit 1 set. A subsequent manual brightness or temperature change cancels a pending Auto request.

`CommandQueue` holds pending command types in a fixed bitset and owns its ready, transmitting, and cooldown phases. Taking a batch reserves the transmitter; its cooldown begins at TX_DONE. Repeated requests of the same type coalesce into the latest shared state, while distinct required operations cannot overwrite one another. A combined power-off, presence-disable, and timeout change therefore sends `0x02`, `0x03`, then `0x05` in separate batches. New requests during transmission remain pending for the next batch.

The ultrasonic select maps Disabled to a cleared presence bit, preserving the timeout. Its other four options enable presence and select the corresponding duration. Changing enable queues settings; changing duration queues the timeout command. Received state maps both fields back to the same select.

The diagnostic **Resend current state** button queues power, settings, and timeout, including settings and timeout while the lamp is off.

The component encodes the complete one- or two-packet batch, then submits it in one `LR1121Radio::send_batch()` call. The driver copies the frames before starting transmission and rejects submissions while busy. **Command sent** is published and the cooldown starts only after every packet reports TX_DONE. New HA requests can update the pending state while that batch is in flight. Received frames cannot overwrite pending commands.

The master light reflects received state under a scoped guard, preventing the resulting callbacks from submitting another request. Its `local_write_` flag preserves that distinction until ESPHome's deferred `write_state()` callback. That callback also captures the final target ESPHome installs after `update_state()` at transition completion. Non-transitioning requests receive canonical integer brightness and temperature values. Number/select publication updates their entities without invoking controls. Nested guards restore their previous value when leaving scope; reflection does not change the radio lifecycle.

Polling also checks for divergent ESPHome current and remote values, covering a newly requested fade before ESPHome sets its transformer-active flag on the first loop.

At boot, master power/temperature preferences, the saved selection, and configured initial brightness restore requested settings without sending settings or power commands. The saved radio link includes address, channel, and timeout; the timeout occupies the former packet-options byte, preserving preference version 2's eight-byte layout. Local controls require a valid received state after boot or recovery, so UI defaults cannot overwrite the sensor's actual configuration. Recovery and starting discovery invalidate the received baseline while retaining requested settings for display. Unchanged status replies do not republish entities.

## LR1121 scheduling and recovery

`BridgeLifecycle` owns command eligibility. Initialization leads to `AWAITING_STATE` for a known link or `DISCOVERING` for a new link. A valid received baseline or completed discovery leads to `ACTIVE`, the only state accepting local commands. Failure enters recovery; retries initialize the radio and require a new baseline. Initialization and recovery each have a discovery variant, so an interrupted scan resumes scanning while a learned link resumes normal reception. Discovery channel changes, listening, and dwell expiry also use explicit phases.

The component loop advances the driver, handles readiness and TX completion, then dispatches discovery or normal reception. Normal processing gives pending commands priority, then receives a packet and services status polling. The command queue and status poll's transmitting phases identify which request owns the submitted batch and prevent further processing until completion is handled. TX_DONE advances whichever request is transmitting. Recovery and discovery cancel both requests, so a late completion cannot revive canceled work. During discovery, the driver finishes any already-submitted batch before configuring reception on the first scan channel.

The driver advances up to four SPI transactions per ESPHome loop call. Reset pulse timing, startup, BUSY waits, calibration, TX completion, RX reads and rearming all use timed states. Individual SPI transfers remain synchronous and bounded to 32 bytes at 1 MHz; the only explicit delay is one microsecond for NSS-to-BUSY propagation. The component keeps its loop enabled to service the radio, checking commands and received frames every 50 ms. ESPHome's `PollingComponent` schedules periodic lamp status queries independently, while named scheduler timeouts handle recovery retries and discovery dwell periods.

The LR1121 driver registers with ESPHome's SPI bus and uses generated GPIO objects for CS, reset, BUSY and IRQ. Each transfer releases CS and the bus before waiting for the radio, allowing other SPI devices to share the bus. Radio recovery reuses the registered device without resetting or freeing the shared bus. ESPHome logs SPI transfer errors; the radio's response checks and BUSY/TX deadlines remain responsible for detecting failed operations because the SPI transfer API has no error return.

Faults stop the current operation and schedule radio reinitialization with an exponential retry delay of 1–30 seconds. The bridge keeps Wi-Fi/API connectivity and its saved address, channel and timeout. Successful initialization resets the retry delay, restores passive reception or restarts an interrupted discovery, and schedules a fresh lamp-state query. In-flight and pending commands are dropped so recovery cannot replay stale actions. Failed initialization also retries instead of permanently marking the component failed.

## LR1121 status polling

`StatusPoll` owns the refresh/read cycle with explicit delay, transmission, and reply-wait phases. It handles PID matching, read retry limits, wrap-safe deadlines, cancellation, and consecutive failure counts. Only its `STATE` result permits applying a lamp observation; the component handles radio I/O and defers new requests while commands or light transitions are pending.

Status polling uses command `0x04`, with a five-second default cycle. It starts after initialization or discovery, and local commands or received controller requests bring the next refresh forward to 500 ms. Polling yields to pending commands and light transitions.

Observed ACKs can contain an earlier command's state, consistent with the lamp preloading its reply before processing the triggering request. This includes an old ON immediately after HA turns the lamp OFF. Each polling cycle therefore:

1. Sends a refresh query and waits for a decoded CRC-valid reply with the matching two-bit packet ID. Its state is discarded, regardless of which supported command it carries. Replies rejected by the ordinary decoder cannot advance the polling cycle.
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

The [native coordinator suite](../tests/README.md) runs the production component, master light adapter, settings entities, protocol, and LR1121 driver together against a scripted chip and minimal ESPHome interfaces. It checks packet ordering, command/poll arbitration, interrupted batches, discovery and recovery, publication guards, retained presence-wake settings, proportional dimming, and transition handling. Time and packet arrivals are controlled by the tests. It complements the isolated lamp-settings, command-queue, lifecycle, and polling tests; real framework interpolation and RF behavior remain covered by compilation and opt-in hardware checks.

**Command sent** means the radio completed transmission and reported TX_DONE, not that a visible lamp change was confirmed. **Lamp status received** means a refresh/read polling cycle returned validated lamp state.

The opt-in [pytest hardware suite](../tests/hardware/) sends native API commands and checks fresh lamp readback in DEBUG logs. It covers independent stored brightness, color-temperature conversion, proportional master dimming, explicit selection, OFF retention with presence enabled, zero master brightness followed by plain ON, transitions, combined power/presence/timeout changes, full resend while off, and all ultrasonic timeout options. It also checks native API brightness/temperature values. These tests operate the lamp and require an explicit `--halo2-device`; ordinary test runs skip them. Readback retention checks do not measure the physical presence pause or trigger a wake.

A separate presence observation on 2026-10-02 used the original profile: Both selected, front/back 10% / 10%, 2700 K, presence enabled, and a five-minute timeout. Master OFF retained that complete profile. A matching ON readback arrived about 40 seconds after OFF, with both sections still selected at 10%; the observer had issued no wake command. The original power/settings were verified afterward. This elapsed time includes sensor response and polling, so it does not precisely measure the manufacturer's documented 30-second pause.

The original controller sends settings snapshots, so a later valid request can recover missed updates. LR1121 polling also catches autonomous presence changes, Auto brightness adjustments, and missed requests. Collisions, range, or a disconnected lamp can delay synchronization; polling retains the last known state until a reply arrives.

Earlier Waveshare firmware was exercised on hardware for discovery, saved-link restoration, native API control, front/back operation, grouped on/off commands, Auto brightness, lamp status reception, and all ultrasonic select options. Combined power/settings/timeout requests were verified by lamp readback. The master-light model passed native regression tests, compiled with the pinned ESPHome version, and was uploaded through encrypted OTA on 2026-10-02. All 25 updated hardware cases passed on `screenbar-halo2.lan`, including OFF retention with presence enabled in Front, Back, and Both modes, proportional dimming, and explicit transitions. The original lamp settings were restored and verified afterward. The [protocol evidence and limitations](PROTOCOL.md#capture-evidence-2026-09-28) distinguish prior checks from physical inactivity timing and other untested behavior. CI runs the native tests and compiles the Waveshare configuration using the pinned Podman image. A successful build or TX_DONE alone does not establish lamp acknowledgement or RF isolation measurements.
