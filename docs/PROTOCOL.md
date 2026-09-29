# ScreenBar HALO 2 radio protocol

This is a reverse-engineered reference for the ScreenBar HALO 2 controller/lamp link, current through commit `5d9cb5f` and hardware captures on 2026-09-28. It covers the ordinary control/status traffic understood by this project, not a complete BenQ specification. Results from one lamp/controller pair do not establish compatibility with every hardware or firmware revision.

The implementation is in [`halo2_protocol.h`](../components/halo2/halo2_protocol.h), [`halo2.cpp`](../components/halo2/halo2.cpp), and [`lr1121_radio.h`](../components/halo2/lr1121_radio.h). See [implementation notes](IMPLEMENTATION_NOTES.md) for ESPHome scheduling and entity behavior.

## Evidence and terminology

| Label | Meaning |
|---|---|
| Confirmed | Captured with a valid CRC or exercised against the lamp; the stated observation is narrower than a complete command specification |
| Reported | Described by another source, without an equivalent local verification |
| Bridge policy | A choice or restriction in this implementation, not necessarily a requirement of the lamp |
| Unknown | Not established by the available evidence |

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

All currently supported requests carry a full ten-byte snapshot, including status requests. A field's presence does not mean every command applies it.

| Payload offset | Frame offset | Size | Meaning |
|---|---|---|---|
| 0 | 1 | 1 | Command |
| 1 | 2 | 1 | Control bits, below |
| 2 | 3 | 1 | Front brightness, integer percent `1..100` |
| 3–4 | 4–5 | 2 | Front/logical shared color temperature in kelvin, big-endian |
| 5 | 6 | 1 | Back brightness, integer percent `1..100` |
| 6–7 | 7–8 | 2 | Back color-temperature field, big-endian |
| 8 | 9 | 1 | Ultrasonic inactivity timeout index `0..3` |
| 9 | 10 | 1 | Observed constant `02`; its purpose is unknown |

Brightness fields retain nonzero values while power is off. The bridge never uses zero brightness as a wire-level OFF command. It represents temperature as one shared setting, writes both fields identically, and rounds outgoing values to 25 K steps within 2700–6500 K (`0A 8C`–`19 64`).

During Auto adjustment, the rear temperature field has been observed to lag the front field. The bridge takes the front field as authoritative. Two fields in the packet do not by themselves establish support for independently controlled color temperatures; unequal-temperature command behavior has not been characterized.

### Control byte

| Bits | Mask | Meaning and evidence |
|---|---|---|
| 0 | `01` | Global power, confirmed |
| 1 | `02` | Auto brightness request when used with command `03`, confirmed; persistent mode/status semantics unknown |
| 2 | `04` | Favorite, reported upstream; save/recall semantics unverified and unsupported |
| 4–3 | `18` | Two-bit lamp selection: `0` front only, `1` back only, `2` both; `3` unknown |
| 5 | `20` | Ultrasonic presence detection enabled, confirmed; this is not an occupancy reading |
| 6–7 | `C0` | Unknown |

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
| `0A` | Pairing, reported upstream | Rejected by the current ordinary-state decoder; never transmitted |
| `0B` | One CRC-valid capture during presence-button interaction; associated with the enable bit clearing, exact command semantics unknown | Rejected by the current decoder; never transmitted |
| Others | Unknown | Rejected |

Known command interactions:

- `03` alone does not turn on a powered-off lamp. The bridge sends `03`, then `02` with power set, to apply settings and turn it on.
- `03` applies brightness only to selected sections, including while global power is off. A check that deselected the back section with 80%, then sent 40% while it remained deselected, left its stored brightness at the previous 62%. After an OFF fade, that stored level can instead reflect the last transmitted fade sample. The bridge retains the requested brightness of unselected sections for their next ON and records the raw lamp readback separately.
- `02` with power clear turns the lamp off. A hardware check also showed that this command did **not** apply a simultaneous presence-disable bit or changed selection. Those settings need their own `03`.
- `05` applies the timeout. All four durations have been written by the bridge and read back from the lamp, including a timeout change while power and presence detection were off.
- Auto is an action in the bridge: set control bit 1 for a `03` request, then observe resulting brightness/temperature through polling. A durable Auto-enabled state is not decoded or exposed.

These observations do not establish the complete write mask of every command. In particular, whether `03` also applies the timeout, or `05` applies additional settings, has not been isolated. The bridge sends complete snapshots and uses the verified command for each requested effect.

### Ultrasonic timeout

| Payload byte 8 | Duration |
|---|---|
| `00` | 3 minutes |
| `01` | 5 minutes |
| `02` | 10 minutes |
| `03` | 15 minutes |

**Wire value `00` means three minutes, not disabled.** Disabling the sensor clears control bit 5 using `03`, retaining the last timeout. HA combines these independent fields into one select: Disabled, 3 minutes, 5 minutes, 10 minutes, or 15 minutes. Choosing a duration enables presence detection and changes the timeout when needed.

The inactivity timer runs in the lamp, not in ESPHome or HA. No occupancy measurement or remaining countdown has been identified in this payload.

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

The bridge's policy is:

1. Send a `04` refresh request. Require a CRC-valid reply with No-ACK set and the matching two-bit PID, but discard its state.
2. Wait 500 ms, then send another `04` request with the next PID.
3. Accept a matching reply as current state only when its command is `04`. If another command is still queued, wait and read again, with at most three read attempts after the initial refresh.

The reply deadline is 200 ms from TX_DONE. The LR1121 itself opens an approximately 20 ms RX window immediately after transmission using `AutoTxRx`. These are different timers. The 500 ms delay, retry count, polling interval, and one-second inter-batch command cooldown are **bridge policies**, not measured protocol minima.

Normal light/settings commands are not automatically retransmitted until acknowledged. TX_DONE establishes local transmission only. The following status cycle reconciles actual lamp state; three timed-out cycles produce a warning and retain the last known state. Polling pauses while local commands or light transitions are pending. The bridge cancels stale polling cycles before transmitting new local commands.

The bridge rotates its own two-bit PID through `0..3`. There is no independent sender ID, timestamp, or wider transaction counter in the decoded payload. Simultaneous controller/bridge traffic, collisions, PID reuse, and duplicate-request suppression are not fully characterized. The refresh/read procedure reduces stale-state problems; it is not a proof of freshness under every possible interleaving.

While listening to the original controller, the bridge must not send ACKs or automatically switch from RX to TX. Otherwise it could interfere with the lamp's own response. The LR1121 driver disables automatic direction switching before passive reception.

## Discovery and decoder limits

Discovery is passive address recovery, not pairing. The driver uses one alternating preamble byte as a temporary sync word, captures the following data, and tries every bit alignment for an address plus valid request. Three matching captures on the same channel are required before saving a link. That threshold is a bridge policy. See [discovery implementation](IMPLEMENTATION_NOTES.md#lr1121-address-discovery).

The current decoder requires:

- Exactly 14 air-buffer bytes, a zero leading PCF bit, and a ten-byte advertised payload.
- A valid software CRC for the selected or candidate address.
- Command in `00..05`, timeout in `00..03`, and suffix `02`.
- Selection `0..2`, both brightness values in `1..100`, and the front temperature in `2700..6500` K.
- No-ACK clear for discovery; ordinary decoding also allows replies, which the component accepts only through its polling rules.

The decoder does **not** require a 25 K receive step, equal temperature fields, a valid rear temperature, zero favorite/unknown control bits, or zero trailing packing bits. It does not retain the rear temperature or favorite/Auto status. CRC-valid controller requests accepted by this filter, including `00`, `01`, and `05`, are currently treated as state snapshots; their exact freshness semantics remain incomplete.

Rejected does not mean invalid BenQ traffic. The observed `0B` packet is one concrete example of a valid-CRC frame outside the decoder's supported command range. Pairing and other payload lengths may require different validation rules.

## Open questions

| Area | What remains unknown | Evidence needed |
|---|---|---|
| Pairing | Upstream reports `0A` and address `E2 08 00 B0`; normal-address assignment/transfer and any encoding are unverified. Encryption during pairing is a hypothesis, not a finding. | Capture both directions through an entire pairing exchange and compare the resulting normal link |
| Additional commands | Meaning of `01`, complete wake/sleep behavior, and whether `0B` is specifically a presence command | Repeat each controller action independently; correlate requests, replies, and physical effects |
| Favorites | Bit 2's action/status meaning, save versus recall, and where presets are stored | Separate captures for saving and recalling different presets |
| Auto mode | Whether bit 1 is only a trigger or also a durable state, and how to identify completion | Follow replies while changing ambient light and while returning to manual control |
| Field write masks | Which fields `02`, `03`, `04`, and `05` apply or persist beyond the verified effects | Change one field at a time under each command and obtain fresh lamp replies |
| Temperature | Whether unequal front/back fields can ever control independent temperatures; the exact cause of reply-field lag | Controlled unequal-temperature requests plus observations of both light outputs |
| Unknown bits/values | Control bits 6–7, selection `3`, suffix `02`, timeout values above `3`, and other payload lengths | Captures from other functions/revisions before relaxing decoder checks |
| ACKs and timing | Payload queue depth, minimum settle time, controller retransmission/deduplication rules, and behavior with overlapping PIDs | Timestamped bidirectional captures with controlled loss, duplicates, and concurrent traffic |
| Presence timing | Exact timer reset/retrigger rules, countdown/occupancy telemetry, and persistence across lamp power loss | Timed motion/no-motion and power-cycle observations alongside packet capture |
| Channels and revisions | Channel choice/hopping, multi-lamp reply arbitration, and compatibility across product revisions | Captures on the other frequencies and from additional paired units |

No cryptographic integrity field or encrypted payload is present in the ordinary format implemented here; its CRC detects transmission errors and does not authenticate the sender. This observation says nothing conclusive about pairing. ESPHome's encrypted native API and OTA are separate links.

## Extending this reference

Record the controller action, before/after physical state, frequency, direction/PID, complete raw frame, and independently checked CRC. Keep capture observations separate from proposed meanings, and mark generated examples as generated. When a field mapping changes, update the codec and this document together.

For unsupported commands, capture before `decode_frame()` filters the packet. The normal `Status RX` verbose log is emitted only during an active poll, so it is not a complete passive sniffer. The timeout investigation temporarily logged every received air buffer and disabled bridge polling while the controller was exercised. A `decoded NO` log alone cannot distinguish a bad CRC from an unsupported field or command.
