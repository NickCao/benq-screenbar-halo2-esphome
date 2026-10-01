# ScreenBar HALO 2 radio protocol

This is a reverse-engineered reference for the ScreenBar HALO 2 controller/lamp link, current through hardware captures on 2026-09-30. It covers the ordinary control/status traffic understood by this project, not a complete BenQ specification. Results from one lamp/controller pair do not establish compatibility with every hardware or firmware revision.

The implementation is in [`halo2_protocol.h`](../components/halo2/halo2_protocol.h), [`halo2.cpp`](../components/halo2/halo2.cpp), and [`lr1121_radio.h`](../components/halo2/lr1121_radio.h). See [implementation notes](IMPLEMENTATION_NOTES.md) for ESPHome scheduling and entity behavior.

## Evidence and terminology

| Label | Meaning |
|---|---|
| Confirmed | Captured with a valid CRC or exercised against the lamp; the stated observation is narrower than a complete command specification |
| Reported | Described by another source, without an equivalent local verification |
| Bridge policy | A choice or restriction in this implementation, not necessarily a requirement of the lamp |
| Unknown | Not established by the available evidence |

A valid CRC establishes a coherent frame for the learned address; it does not establish which fields a command applies. Controller actions, lamp readback, and physical observations are identified separately below.

The [independent command probes](PROTOCOL_EXPERIMENTS.md) cover 120 command cases and 12 timing sequences without controller input. Their [captured frame evidence](captures/protocol-probes-2026-09-30.json) includes actual requests and repeated fresh lamp replies. Those results establish reported setting changes; they do not establish physical output or internal preset storage.

Earlier research comes from [kuzmin-no's integration](https://github.com/kuzmin-no/BenQ_ScreenBar_HALO_2_HA_integration#technical-details). Its wake/sleep and pairing labels are useful leads, but its fixed payload ending `01 02` is incomplete: the penultimate byte is the configurable inactivity timeout. The [BenQ product information](https://www.benq.com/en-sg/campaign/elevate-with-halo.html) independently lists the four available durations. Neither source documents the complete command behavior below.

All byte values in hex examples are hexadecimal. Bit 0 is the least significant bit. Payload offsets start at the command byte; canonical-frame offsets additionally include the one-byte PCF representation.

## Radio link

| Item | Working implementation | Evidence or limit |
|---|---|---|
| Modulation | 2.4 GHz GFSK, 125 kbit/s | Interoperates with the tested controller and lamp; this is not LoRa framing |
| Channel numbers | Frequency in MHz = `2400 + channel`; candidates `5`, `46`, `75` | The tested link uses 2405 MHz; the other candidates come from upstream research and are scanned, not locally verified active links |
| Address | Four bytes, reused as the LR1121's 32-bit sync word | Confirmed from discovery, CRCs, and successful control |
| Byte order | Reverse the configured/register-order address for transmission and CRC | For example, `9C EA BB 86` becomes `86 BB EA 9C` on air |
| LR1121 tuning | 160 kHz frequency deviation, Gaussian BT=0.5, 467 kHz RX bandwidth, 0 dBm TX | Tested bridge settings; not measurements of the original transmitter's exact deviation, filter, or output power |
| Preamble | Alternating bits; the bridge transmits 32 preamble bits | Exact controller/ACK preamble lengths have not been characterized; RX preamble detection is disabled to receive short ACKs |
| Whitening | Disabled | The decoded ordinary frames are unwhitened |

Addresses are learned from an already paired controller; the compile-time reference address is not a universal lamp address. The bridge stays on the learned channel. Channel selection, hopping, and multi-lamp ACK behavior are unknown.

The [Semtech LR1121 manual, sections 7.2 and 8.4–8.5](https://files.waveshare.com/wiki/Core1121/UserManual_LR1121_v1_2.pdf) describes the transceiver's packet engine. The ScreenBar fields below are supplied by our software.

## Framing and packet control field

Ordinary traffic has this bit layout, transmitted most significant bit first:

```text
alternating preamble | address (32 bits) | PCF (9 bits) | payload (80 bits) | CRC (16 bits)
```

The nine-bit PCF consists of six payload-length bits, two packet-ID bits, and one No-ACK bit. For a ten-byte payload its leading bit is zero. The codec removes that leading bit to form a convenient **13-byte canonical frame**:

```text
PCF low byte (1) | payload (10) | CRC high byte (1) | CRC low byte (1)
```

| Canonical PCF bits | Meaning |
|---|---|
| 7–3 | Remaining five length bits: `10` for the supported payload |
| 2–1 | Two-bit packet ID, `0..3` |
| 0 | No-ACK: observed requests clear it; lamp replies set it |

Requests use `0x50 | ((pid & 3) << 1)`: `50`, `52`, `54`, or `56`. Corresponding reply PCFs are `51`, `53`, `55`, or `57`. No-ACK is a packet-control flag, not an authenticated sender identity. The bridge interprets it as direction for the traffic described here.

The LR1121 captures the **105 bits after the address** in a 14-byte buffer. The first bit is the PCF's leading zero; the last byte contains one meaningful CRC bit and seven trailing bits. The bridge transmits those trailing bits as zero and ignores them on reception, where they need not be zero.

For air-buffer bytes `air[0..13]`, canonical bytes are:

```text
frame[i] = ((air[i] << 1) | (air[i + 1] >> 7)) & 0xFF, for i = 0..12
```

There is no additional LR1121 length byte, address-filter byte, whitening sequence, or hardware-generated CRC in this representation.

## Application payload

All supported requests carry a full ten-byte snapshot, including status and favorite `07`/`08` requests. A field's presence does not mean every command applies it.

| Payload offset | Frame offset | Size | Meaning |
|---|---|---|---|
| 0 | 1 | 1 | Command |
| 1 | 2 | 1 | Control bits, below |
| 2 | 3 | 1 | Front brightness, integer percent `1..100` |
| 3–4 | 4–5 | 2 | Front/logical shared color temperature in kelvin, big-endian |
| 5 | 6 | 1 | Back brightness, integer percent `1..100` |
| 6–7 | 7–8 | 2 | Back color-temperature field, big-endian |
| 8 | 9 | 1 | Ultrasonic inactivity timeout index; durations mapped for `0..3`, `04` was also retained after a probe but its duration is unknown |
| 9 | 10 | 1 | Controller/status value `02`; manual `03` also accepted suffixes `00`, `01`, and `03`, with purpose still unknown |

Brightness fields retain nonzero values while power is off. The bridge never uses zero brightness as a wire-level OFF command. It represents temperature as one shared setting, writes both fields identically, and rounds outgoing values to 25 K steps within 2700–6500 K (`0A 8C`–`19 64`).

During Auto adjustment, the temperature fields can differ. In the independent probes, the first field changed to 4000 K while the rear field retained the manual 4200 K value, including after passive listening. The bridge takes the front field as authoritative. The unequal-temperature experiment below found that command `03` retained both fields in replies while visible color followed the first field, including with only the rear section selected. The cause and purpose of unequal fields remain unknown.

### Unequal-temperature experiment, 2026-09-29

A temporary probe build sent command `03` with opposite temperature extremes, manual brightness at 70%, presence disabled, and either both sections or only the rear selected. It changed the rear payload field directly, bypassing the bridge's shared-temperature conversion. Fresh replies came from the normal refresh/read cycle; an independent bitwise CRC check against the learned radio address passed for all 16 distinct captured status frames.

| Selection | First temperature field | Rear temperature field | Fresh reply fields | User's visual observation |
|---|---|---|---|---|
| Both | 2700 K | 6500 K | 2700 / 6500 K | Both warm/yellow |
| Both | 6500 K | 2700 K | 6500 / 2700 K | Both cool/white |
| Rear only | 2700 K | 6500 K | 2700 / 6500 K | Rear warm/yellow |
| Rear only | 6500 K | 2700 K | 6500 / 2700 K | Rear cool/white |

Captured canonical status frames, including PCF and CRC, for those four cases:

```text
57 04 11 46 0A 8C 46 19 64 00 02 6C 8E
57 04 11 46 19 64 46 0A 8C 00 02 8C E6
57 04 09 46 0A 8C 46 19 64 00 02 16 8F
57 04 09 46 19 64 46 0A 8C 00 02 F6 E7
```

The observed output supports treating the first field as shared temperature for manual `03` settings on this unit. Retaining the rear field in a reply does not establish that it controls an independent rear output. Its purpose under other commands or modes remains unknown; this experiment does not establish that it is unused throughout the protocol.

### Control byte

| Bits | Mask | Meaning and evidence |
|---|---|---|
| 0 | `01` | Global power, confirmed |
| 1 | `02` | Auto request under `03`, retained in later replies until cleared by a manual request; `06` also triggers Auto with this bit clear. It does not reliably indicate active adjustment or completion |
| 2 | `04` | Favorite flag, set/cleared in replies by manual `03` as well as Favorite requests. Not required for tested `07`/`08` setting changes; other internal/status semantics remain unknown |
| 4–3 | `18` | Two-bit selection: `0` front only, `1` back only, `2` both. `3` was retained under `03`/`07`/`08`, with neither brightness field updated; visible behavior remains unverified |
| 5 | `20` | Ultrasonic presence detection enabled, confirmed; this is not an occupancy reading |
| 6–7 | `C0` | Retained in replies when supplied under manual `03`; functional meaning unknown |

Selection is a two-bit enumeration, not two independent on/off bits. There is no confirmed “neither selected” value: turning everything off clears global power while retaining a valid selection. The bridge ignores control bits 2, 6, and 7 on reception and clears them in outgoing packets. It does not preserve unknown/favorite bits.

### Commands

| Command | Known or reported behavior | Current bridge handling |
|---|---|---|
| `00` | Reported controller wake/contact; captured during controller use, but side effects and snapshot freshness are not fully characterized | Decoded if the remaining fields are valid; never transmitted |
| `01` | Unknown; no established function | Allowed by the decoder's `00..05` range; never transmitted |
| `02` | Confirmed global power command | Transmitted for power changes |
| `03` | Confirmed selection, brightness, temperature, and presence-enable settings; bit 1 triggers Auto adjustment | Transmitted for these settings |
| `04` | Confirmed lamp-status request/reply | Transmitted by polling |
| `05` | Confirmed timeout update; also seen when the controller becomes idle, consistent with upstream's sleep label | Transmitted to save the timeout |
| `06` | Confirmed Auto brightness/first-temperature adjustment in controlled probes; does not apply conflicting snapshot settings | Rejected by the current decoder; never transmitted by normal firmware |
| `07` | Controller Favorite recall; applies selection, selected brightness, temperature fields, and presence in tested ON/OFF states, preserving power and timeout; Favorite bit is not required for these changes | Decoded as a controller snapshot; never transmitted by normal firmware |
| `08` | Controller Favorite save by long press; also applies the same tested live settings as `07`; internal preset storage remains unverified | Decoded as a controller snapshot; never transmitted by normal firmware |
| `0A` | Pairing, reported upstream | Rejected by the current ordinary-state decoder; never transmitted |
| `0B` | Confirmed presence-enable update; other tested snapshot fields remained unchanged | Rejected by the current decoder; never transmitted |
| Others | Unknown | Rejected |

Known command interactions:

- `03` alone does not turn on a powered-off lamp. The bridge sends `03`, then `02` with power set, to apply settings and turn it on.
- `03` applies brightness only to selected sections, including while global power is off. A check that deselected the back section with 80%, then sent 40% while it remained deselected, left its stored brightness at the previous 62%. After an OFF fade, that stored level can instead reflect the last transmitted fade sample. The bridge retains the requested brightness of unselected sections for their next ON; debug logs show the raw lamp readback.
- `02` with power clear turns the lamp off. A hardware check also showed that this command did **not** apply a simultaneous presence-disable bit or changed selection. Those settings need their own `03`.
- `05` applies the timeout. All four durations have been written by the bridge and read back from the lamp, including a timeout change while power and presence detection were off.
- Auto is an action in the bridge: set control bit 1 for a `03` request, then observe resulting brightness/temperature through polling. A durable Auto-enabled state is not decoded or exposed.

The [independent field matrix](PROTOCOL_EXPERIMENTS.md#fields-applied-by-commands) narrows which snapshot fields each command changes:

| Command | Applied fields in the probes | Fields left unchanged |
|---|---|---|
| `02` | Global power | Selection, brightness, temperature fields, presence, timeout |
| `03`, `07`, `08` | Selection, selected-section brightness, temperature fields, presence | Global power, timeout |
| `04` | Status query | All supplied snapshot settings |
| `05` | Timeout | Global power, selection, brightness, temperature fields, presence |
| `0B` | Presence enable | Global power, selection, brightness, temperature fields, timeout |

These are reported-state observations under the documented conditions, not a complete specification of every side effect. In particular, some `05` requests following other commands were acknowledged but did not apply their timeout; a retry after readback did. [Sequencing details](PROTOCOL_EXPERIMENTS.md#timeout-sequencing-exception) do not establish a universal delay threshold. The bridge sends complete snapshots and uses the verified command for each requested effect.

### Favorite save/recall experiment, 2026-09-30

A temporary passive build disabled bridge transmissions while the user performed two cycles with the original controller: hold Favorite to save, change brightness, then tap Favorite to recall. The user confirmed that both recalls visibly restored the saved brightness. The capture contains 79 receive buffers: 78 pass an independent bitwise CRC check, including all four favorite commands; one ordinary `04` request has a one-bit CRC mismatch and is excluded from the findings.

The link used 2405 MHz and register-order address `4F 23 8C CE` (on-air/CRC order `CE 8C 23 4F`). The controller snapshots indicated both sections selected and powered on, both temperature fields at 2700 K, presence enabled, and timeout byte `01` (five minutes). Only brightness was varied. Percentages below are controller payload values; the user confirmed the visible recall behavior, and no lamp replies were captured in this passive experiment.

| Cycle | Saved front/back brightness | Last manual brightness before recall | Recall payload brightness | Observed lamp result |
|---|---|---|---|---|
| 1 | 25% / 25% | 2% / 2% | 25% / 25% | Saved brightness restored |
| 2 | 48% / 48% | 35% / 35% | 48% / 48% | Saved brightness restored |

Captured canonical frames, including PCF and independently verified CRC (timestamps are UTC on 2026-10-01, corresponding to the evening of 2026-09-30 locally):

| UTC time | Action | Canonical frame |
|---|---|---|
| 00:24:29.363 | Save cycle 1 | `54 08 35 19 0A 8C 19 0A 8C 01 02 10 AE` |
| 00:24:35.721 | Recall cycle 1 | `52 07 35 19 0A 8C 19 0A 8C 01 02 B7 EB` |
| 00:24:44.976 | Save cycle 2 | `56 08 35 30 0A 8C 30 0A 8C 01 02 32 B4` |
| 00:24:47.994 | Recall cycle 2 | `54 07 35 30 0A 8C 30 0A 8C 01 02 C4 B6` |

All four are requests (No-ACK clear). Their control byte is `35`, which adds Favorite bit `04` to the ordinary manual-settings value `31`. Following each save or recall, controller `04` requests retain `35`; the next manual `03` request clears the bit to `31`. No lamp replies were captured in this experiment, so this does not establish an active-favorite indicator in lamp replies.

Recall requests contain the previously saved settings, even after manual settings have changed. These controller captures establish the command mapping and payload contents on this pair. The follow-up below tests which brightness values `07` applies. This passive capture did not isolate preset storage, flag necessity, field write masks, or persistence; the later [independent probes](PROTOCOL_EXPERIMENTS.md#favorites-and-selection) tested flag necessity for live setting changes and additional fields.

#### Conflicting recall payload experiment, 2026-09-30

A temporary firmware action sent controlled `08` and `07` requests using the same learned address and channel. For each case, ordinary `03` settings first established the save brightness; `08` then carried that brightness with Favorite set. Another `03` changed brightness to 35%, followed by a `07` request carrying a different brightness with Favorite set. Both sections remained on and selected at 2700 K, presence was enabled, and the timeout remained five minutes. All favorite requests used control `35`.

Two fresh lamp readbacks were required after every step, using the normal refresh/read polling sequence to discard queued ACK state. The original controller was not used to generate these probe requests. Independent bitwise CRC checks passed for every recorded probe transmission and lamp reply.

| `08` save request brightness | Manual brightness before recall | `07` recall payload brightness | Two fresh lamp readbacks, front/back |
|---|---|---|---|
| 25% / 25% | 35% / 35% | 48% / 48% | 48% / 48%, twice |
| 60% / 60% | 35% / 35% | 65% / 65% | 65% / 65%, twice |

Transmitted probe frames and representative accepted lamp replies, in canonical form with independently verified CRC (UTC on 2026-10-01):

| UTC time | Action or reply | Canonical frame |
|---|---|---|
| 01:13:27.590 | `08` with 25% | `54 08 35 19 0A 8C 19 0A 8C 01 02 10 AE` |
| 01:13:42.597 | `07` with 48% | `56 07 35 30 0A 8C 30 0A 8C 01 02 64 05` |
| 01:13:47.385 | Fresh 48% status reply | `57 04 35 30 0A 8C 30 0A 8C 01 02 0D 83` |
| 01:13:57.666 | `08` with 60% | `50 08 35 3C 0A 8C 3C 0A 8C 01 02 59 DC` |
| 01:14:12.622 | `07` with 65% | `52 07 35 41 0A 8C 41 0A 8C 01 02 75 4A` |
| 01:14:17.488 | Fresh 65% status reply | `53 04 35 41 0A 8C 41 0A 8C 01 02 1C CC` |

**Confirmed:** under these conditions, `07` applies its supplied brightness even when it differs from the preceding `08` save request. The resulting brightness follows the recall payload. Together with the controller captures, this establishes that the controller supplies the favorite brightness on recall. It does not prove exclusive controller-side storage, rule out a lamp-side copy, or isolate whether `08` stores anything in the lamp. Power, selection, temperature, presence, and timeout were held constant in this experiment; their effects were subsequently tested in the [independent field matrix](PROTOCOL_EXPERIMENTS.md#fields-applied-by-commands).

Lamp `04` replies retained control `35` after `07`/`08`, including across bridge status requests whose Favorite bit was clear. Manual `03` settings with Favorite clear changed replies back to `31`. Later probes applied Favorite settings with the bit clear and also set it using manual `03`; its internal role and power-loss behavior remain unverified.

Cleanup replayed `08` for the last controller-captured 48% favorite, restored the starting lamp state (both on at 9%, 2700 K, presence enabled, five-minute timeout), and restored normal firmware. Two fresh replies after restoration matched the initial state, and the temporary API action was absent.

### Bridge handling of Favorite traffic

The agreed integration scope is to decode incoming controller `07`/`08` snapshots so the bridge follows controller actions, then reconcile actual lamp state through fresh `04` replies. This does not require the bridge to store a favorite, expose save/recall actions, or transmit favorite commands. Knowing whether the lamp keeps another preset copy is unnecessary for following those messages. This is bridge policy, not a statement that the lamp has no preset storage.

The decoder accepts `00..05` plus explicit `07`/`08` entries. Confirmed commands `06` and `0B` remain unsupported by the production decoder; the independent probes did not add runtime support. Favorite requests retain their original command codes and use the ordinary controller-snapshot path: publish the supplied settings without echoing a command, then schedule a fresh lamp poll. Favorite bit `04` is not retained in bridge state. The snapshot does not establish that every supplied field became the lamp's current state; fresh `04` replies reconcile it.

Decoded favorite replies follow the existing status-poll rules. A matching reply can acknowledge the refresh query, but a queued `07`/`08` reply to the read query cannot publish lamp state and instead triggers a delayed retry while attempts remain. Only a matching `04` reply after refresh is accepted as fresh lamp state.

### Ultrasonic timeout

| Payload byte 8 | Duration |
|---|---|
| `00` | 3 minutes |
| `01` | 5 minutes |
| `02` | 10 minutes |
| `03` | 15 minutes |

**Wire value `00` means three minutes, not disabled.** Disabling the sensor clears control bit 5 using `03`, retaining the last timeout. HA combines these independent fields into one select: Disabled, 3 minutes, 5 minutes, 10 minutes, or 15 minutes. Choosing a duration enables presence detection and changes the timeout when needed.

The inactivity timer runs in the lamp, not in ESPHome or HA. No occupancy measurement or remaining countdown has been identified in this payload.

An independent `05` probe also stored timeout index `04` in fresh replies. Its actual inactivity duration was not measured; the bridge continues to support only the four mapped durations above.

#### Capture evidence, 2026-09-28

The original controller was initially set to five minutes. The user saved 10, 3, 15, then 5 minutes in that order. The following ten-byte payloads were extracted from CRC-valid captures; address, PCF, and CRC are omitted. In these examples the lamp was powered on, both sections were selected at 10% and 2700 K, and presence detection was disabled.

| Saved setting | Controller payload |
|---|---|
| 10 minutes | `05 11 0A 0A 8C 0A 0A 8C 02 02` |
| 3 minutes | `05 11 0A 0A 8C 0A 0A 8C 00 02` |
| 15 minutes | `05 11 0A 0A 8C 0A 0A 8C 03 02` |
| 5 minutes | `05 11 0A 0A 8C 0A 0A 8C 01 02` |

Subsequent `04` controller snapshots carried the same timeout index. Earlier in the same capture, `0B 11 0A 0A 8C 0A 0A 8C 01 02` appeared after snapshots with control `31`; the CRC was valid, but this single event does not establish what `0B` does.

After implementation, each enabled duration and Disabled were confirmed by two lamp status replies and the corresponding native API state, with brightness and temperature unchanged. A separate combined-command check confirmed power OFF, presence OFF, and a 15-minute timeout together, then restored the starting state. These checks establish configuration/readback behavior; they did not measure the elapsed physical inactivity interval for all four durations or persistence across a lamp power cycle.

## CRC

The CRC is 16 bits, polynomial `0x1021`, initial value `0xFFFF`, processed most significant bit first without reflection or a final XOR. It covers, in order:

1. The four address bytes in **on-air order**.
2. The full **nine-bit** PCF, including its leading zero bit.
3. The ten payload bytes.

Preamble, transmitted CRC, and the seven trailing packing bits are excluded. There are no extra zero bits appended to the calculation. The resulting CRC is sent high byte first.

An equivalent bitwise calculation is:

```text
crc = 0xFFFF
for each covered bit, in transmission order:
    feedback = ((crc >> 15) ^ bit) & 1
    crc = (crc << 1) & 0xFFFF
    if feedback:
        crc = crc ^ 0x1021
```

A byte-oriented CRC over the canonical PCF and payload alone will be wrong: it omits the address and the extra PCF bit. The bridge disables the LR1121 packet CRC and constructs/checks this CRC in software because the supported layout includes the sync/address and a non-byte-aligned header. Disabling hardware CRC does not mean accepting unchecked frames.

### Reproducible encoding example

This is a generated example using the public reference address, not a captured device address. It requests status with PID 2, power on, both sections selected, presence enabled, front brightness 40%, back brightness 20%, shared 4000 K, and a five-minute timeout.

```text
Address, register order: 9C EA BB 86
Address, on-air order:   86 BB EA 9C
PCF, nine bits:          0 01010100
Payload:                04 31 28 0F A0 14 0F A0 01 02
CRC:                    D7 EF
Canonical frame:        54 04 31 28 0F A0 14 0F A0 01 02 D7 EF
LR1121 buffer:          2A 02 18 94 07 D0 0A 07 D0 00 81 6B F7 80
```

The LR1121 emits the configured preamble and address before that buffer. Only the top bit of its final `80` belongs to the CRC.

## Replies, polling, and ordering

Lamp state has been obtained through replies to requests; unsolicited updates after autonomous changes have not been established. Hearing the original controller is useful but insufficient when the lamp changes itself through presence detection or Auto adjustment. The bridge polls with `04` every five seconds by default.

**Confirmed:** a matching ACK can contain state from before the triggering request, even after the lamp has already acted on a newer command. Packet ID matching alone does not establish payload freshness. A preloaded/delayed ACK payload is the working explanation; exact queue depth and update timing remain unknown.

The [independent timing sequences](PROTOCOL_EXPERIMENTS.md#queued-replies-and-repeated-queries) commonly returned old `04` state, then the preceding `03` reply with new settings, then fresh `04` state, even with requested 500 ms delays. Eight identical status queries using the same PID kept returning the old payload; changing the PID or query contents advanced replies in the tested cases. This is consistent with PID/content-based duplicate handling, but the exact cache rules remain unknown.

The bridge's policy is:

1. Send a `04` refresh request. Require a reply that passes the ordinary decoder, including CRC validation, with No-ACK set and the matching two-bit PID, but discard its state.
2. Wait 500 ms, then send another `04` request with the next PID.
3. Accept a decoded matching reply as current state only when its command is `04`. If another supported command is still queued, wait and read again, with at most three read attempts after the initial refresh. A reply rejected by the decoder cannot advance the polling cycle and may cause a timeout.

The reply deadline is 200 ms from TX_DONE. The LR1121 itself opens an approximately 20 ms RX window immediately after transmission using `AutoTxRx`. These are different timers. The 500 ms delay, retry count, polling interval, and one-second inter-batch command cooldown are **bridge policies**, not measured protocol minima.

Normal light/settings commands are not automatically retransmitted until acknowledged. TX_DONE establishes local transmission only. The following status cycle reconciles actual lamp state; three timed-out cycles produce a warning and retain the last known state. Polling pauses while local commands or light transitions are pending. The bridge cancels stale polling cycles before transmitting new local commands.

The bridge rotates its own two-bit PID through `0..3`. There is no independent sender ID, timestamp, or wider transaction counter in the decoded payload. Simultaneous controller/bridge traffic, collisions, PID reuse, and duplicate-request suppression are not fully characterized. The refresh/read procedure reduces stale-state problems; it is not a proof of freshness under every possible interleaving.

While listening to the original controller, the bridge must not send ACKs or automatically switch from RX to TX. Otherwise it could interfere with the lamp's own response. The LR1121 driver disables automatic direction switching before passive reception.

## Discovery and decoder limits

Discovery is passive address recovery, not pairing. The driver uses one alternating preamble byte as a temporary sync word, captures the following data, and tries every bit alignment for an address plus valid request. Three matching captures on the same channel are required before saving a link. That threshold is a bridge policy. See [discovery implementation](IMPLEMENTATION_NOTES.md#lr1121-address-discovery).

The current decoder requires:

- Exactly 14 air-buffer bytes, a zero leading PCF bit, and a ten-byte advertised payload.
- A valid software CRC for the selected or candidate address.
- Command in `00..05`, `07`, or `08`, timeout in `00..03`, and suffix `02`.
- Selection `0..2`, both brightness values in `1..100`, and the front temperature in `2700..6500` K.
- No-ACK clear for discovery; ordinary decoding also allows replies, which the component accepts only through its polling rules.

The decoder does **not** require a 25 K receive step, equal temperature fields, a valid rear temperature, zero favorite/unknown control bits, or zero trailing packing bits. It does not retain the rear temperature or favorite/Auto status. CRC-valid controller requests accepted by this filter, including `00`, `01`, `05`, `07`, and `08`, are treated as state snapshots and reconciled through fresh polling; their exact field write masks and snapshot freshness remain incompletely characterized.

Rejected does not mean invalid BenQ traffic. The probes established valid `06`/`0B` traffic, retained selection `3` and timeout `04` in lamp replies, and applied manual settings with suffixes other than `02`. Those values remain outside the bridge's supported set; acceptance by the lamp does not establish their complete semantics. Pairing and other payload lengths may require different validation rules.

## Open questions

| Area | What remains unknown | Evidence needed |
|---|---|---|
| Pairing | Upstream reports `0A` and address `E2 08 00 B0`; normal-address assignment/transfer and any encoding are unverified. Encryption during pairing is a hypothesis, not a finding. | Capture both directions through an entire pairing exchange and compare the resulting normal link |
| Additional commands | Meaning of `01`, complete `00`/`05` wake/sleep behavior, and unobserved codes. `06` Auto and `0B` presence-enable effects are now confirmed on this pair | Repeat controller actions independently; correlate requests, replies, and physical effects |
| Favorites | Whether the lamp also stores a preset, additional internal effects of `08`, internal role of bit 2, and persistence. Live setting changes work with the Favorite bit clear | Isolate preset storage and power-loss behavior; test other revisions |
| Auto mode | Continuous regulation, completion, algorithm, and ambient-light response. A retained bit 1 is not a reliable indicator of activity; `06` triggers Auto with it clear | Controlled ambient changes and physical output measurement alongside replies |
| Field write masks | Side effects beyond the tested fields/conditions, and why some acknowledged `05` sequences do not apply the timeout | Repeat controlled sequences with RF-edge timing and distinguish acceptance from application |
| Temperature | Rear field's purpose and cause of unequal values; Auto retained the manual rear field while changing the first, and unequal manual `03` fields did not produce independent visible temperatures | Physical measurement under other commands/modes alongside fresh replies |
| Unknown bits/values | Meaning of retained bits 6–7, visible effect of selection `3`, suffix behavior beyond manual `03`, duration of timeout `04` and any higher values, other payload lengths | Controlled physical observations and captures from other functions/revisions |
| ACKs and timing | Payload/cache depth, exact PID/content duplicate rules, minimum settle time, timeout-sequencing exceptions, controller retransmission rules, and overlapping traffic | Timestamped bidirectional captures with controlled loss, duplicates, and concurrent traffic |
| Presence timing | Exact timer reset/retrigger rules, countdown/occupancy telemetry, and persistence across lamp power loss | Timed motion/no-motion and power-cycle observations alongside packet capture |
| Channels and revisions | Channel choice/hopping, multi-lamp reply arbitration, and compatibility across product revisions | Captures on the other frequencies and from additional paired units |

No cryptographic integrity field or encrypted payload is present in the ordinary format implemented here; its CRC detects transmission errors and does not authenticate the sender. This observation says nothing conclusive about pairing. ESPHome's encrypted native API and OTA are separate links.

## Extending this reference

Record the controller action, before/after physical state, frequency, direction/PID, complete raw frame, and independently checked CRC. Keep capture observations separate from proposed meanings, and mark generated examples as generated. When a field mapping changes, update the codec and this document together.

The DEBUG `RX air frame` log records every normal receive buffer, including unsupported commands and packets that fail validation. PCF, command, and control bytes are extracted directly from the raw frame, so rejection cannot replace them with defaults. A `decoded NO` result alone cannot distinguish a bad CRC from an unsupported field or command; retain the raw bytes and check CRC independently. For passive controller investigations, temporarily disable bridge transmissions and capture these logs alongside the address, channel, and action sequence. The timeout investigation used this approach to avoid introducing bridge polling traffic.
