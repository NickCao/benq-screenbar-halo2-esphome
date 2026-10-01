# Independent protocol probes, 2026-09-30

These experiments used the ESPHome bridge to transmit controlled requests to one paired ScreenBar HALO 2. They ran on 2026-10-01 UTC, corresponding to the evening of 2026-09-30 locally. No original-controller input, visual observation, controlled motion, ambient-light manipulation, or lamp/controller power cycling was involved.

The results cover **120 command cases and 12 timing sequences**. The [portable evidence](captures/protocol-probes-2026-09-30.json) contains actual transmitted frames, baseline state, two equal-payload status confirmations for every command case, and the timing and Auto observations. The [protocol reference](PROTOCOL.md) describes the framing and CRC.

## Method and limits

- Link: 2405 MHz, register-order address `4F 23 8C CE`, on-air/CRC order `CE 8C 23 4F`.
- An isolated ESPHome 2026.9.0 probe build exposed raw ten-byte requests and explicit PID selection. Normal command handling and periodic polling were disabled during each run; passive reception remained active. Production component sources were not changed.
- The usual baseline was power on, both sections selected, front 37%, back 62%, both temperature fields 4200 K, presence disabled, timeout index `01` (five minutes). Power-off and presence-enabled variants are identified in the evidence.
- The field matrix separately requested power off, front-only selection, front 53%, back 81%, first temperature 5100 K, rear temperature 5300 K, presence enabled, and timeout index `03` (15 minutes).
- State conclusions require independently CRC-checked `04` replies with matching PIDs and at least two equal payloads after the test request. Earlier replies, other command replies, and invalid buffers were excluded. Setup commands were separated by complete readback cycles after a grouped setup failed to restore the timeout.
- The journal contains 1,779 transmitted frames, all with valid CRCs, and 1,771 receive buffers, of which 75 failed CRC. Corrupt buffers are not evidence of command semantics. These counts include initial calibration, setup, and cleanup, not just the exported cases.

Readback establishes reported settings, not visible light output or undocumented internal storage. Device timestamps mark transmit submission and receive-buffer consumption; they are not RF-edge timestamps. API and radio-driver overhead prevent interpreting a requested host delay as a measured protocol minimum.

Representative captured canonical frames, including PCF and CRC, are shown below. The readbacks are from completed verification cycles, not necessarily the immediate ACK to the listed request; their PIDs belong to the intervening status queries. Each sample is identified by case name in the evidence file.

| Case | Transmitted request | Fresh status reply |
|---|---|---|
| `auto_compare_cmd06_bit0_power1_conflict1` | `50 06 20 35 13 EC 51 14 B4 03 02 D5 81` | `55 04 11 22 0F A0 1A 10 68 01 02 22 9A` |
| `presence0B_power` | `52 0B 30 25 10 68 3E 10 68 01 02 59 03` | `53 04 31 25 10 68 3E 10 68 01 02 3C D8` |
| `favorite07_flag0_from_power0` | `56 07 31 2B 13 24 47 13 24 02 02 30 C0` | `57 04 30 2B 13 24 47 13 24 01 02 E7 36` |
| `favorite08_flag0_from_power1` | `52 08 31 2B 13 24 47 13 24 02 02 37 36` | `53 04 31 2B 13 24 47 13 24 01 02 5D 52` |
| `unsupported_timeout4` | `50 05 11 35 10 68 3E 10 68 04 02 4E DA` | `51 04 11 25 10 68 3E 10 68 04 02 C4 62` |

## Fields applied by commands

This table summarizes the tested fields. It is not a complete specification of side effects under every condition.

| Command | Power | Selection | Brightness | Temperature fields | Presence enable | Timeout |
|---|---|---|---|---|---|---|
| `02` | Applied | Unchanged | Unchanged | Unchanged | Unchanged | Unchanged |
| `03` | Unchanged | Applied | Selected sections only | Both retained as supplied | Applied | Unchanged |
| `04` | Unchanged | Unchanged | Unchanged | Unchanged | Unchanged | Unchanged |
| `05` | Unchanged | Unchanged | Unchanged | Unchanged | Unchanged | Applied, subject to the sequencing observations below |
| `07` | Unchanged | Applied | Selected sections only | Both retained as supplied | Applied | Unchanged |
| `08` | Unchanged | Applied | Selected sections only | Both retained as supplied | Applied | Unchanged |
| `0B` | Unchanged | Unchanged | Unchanged | Unchanged | Applied | Unchanged |

Power-on `02` was also tested from an OFF baseline with conflicting settings; only power changed. From OFF, `03` applied selection, selected brightness, temperature fields, and presence without turning on the lamp; `05` changed timeout without turning it on. Favorite `07`/`08` likewise applied their settings while power remained off.

For `0B`, each enable case supplied presence ON plus a conflicting power, selection, brightness, temperature, or timeout field. Presence changed while those other fields stayed unchanged. The original captured `0B` payload and an isolated disable request both disabled presence. This establishes a presence-enable command on this pair; it does not establish sensor occupancy reporting or timer-reset rules.

Requests using `00` and `01` changed none of the eight snapshot fields in the tested presence-enabled baseline. Their wake/contact/sleep or other side effects remain unknown; this does not establish that they are no-ops.

## Favorites and selection

Both `07` and `08` applied selection, selected brightness, temperature fields, and presence with Favorite bit `04` either clear or set. They preserved global power and timeout, including when requests supplied power ON from an OFF baseline and a different timeout. The Favorite bit is therefore unnecessary for these reported setting changes on this pair.

Both Favorite commands also followed the manual `03` brightness-selection rule: front-only selection updated only front brightness; rear-only selection updated only rear brightness. The unchanged section retained its previous reported level.

Save `08` has a demonstrated effect on live settings when its payload differs from the current state. Whether it additionally saves a preset internally, and persistence across power loss, remain unknown. The earlier conflicting-recall experiment establishes that `07` uses supplied brightness; these probes do not identify exclusive preset storage ownership.

Manual `03` with Favorite bit set retained that bit in subsequent status replies. Manual `03` with it clear cleared it. A retained Favorite bit alone therefore does not prove that a save/recall operation occurred or that current settings equal a stored preset.

Selection `3` was accepted and retained under `03`, `07`, and `08`. Neither brightness field changed when both were supplied with new values under that selection. Its visible output behavior was not observed, so it is not yet established as a usable “both off” selection.

## Auto adjustment and command `06`

Command `06` triggered brightness and first-temperature changes consistent with Auto adjustment, with presence disabled and with global power either ON or OFF. A conflicting request supplied power OFF, front-only selection, brightness 53%/81%, temperatures 5100/5300 K, presence ON, and timeout index `03`; the resulting state preserved power, selection, presence, timeout, and the rear temperature while calculating different brightness values and a first temperature of 4000 K.

For comparison, `03` with Auto bit `02` set produced similar calculated changes under the same room conditions. Representative final values were:

| Probe | Power | Front/back brightness | First/rear temperature | Status control |
|---|---|---|---|---|
| `06`, ordinary snapshot | ON | 36% / 27% | 4000 / 4200 K | `11` |
| `06`, conflicting snapshot | ON | 34% / 26% | 4000 / 4200 K | `11` |
| `03`, Auto bit set | ON | 36% / 25% | 4000 / 4200 K | `13` |
| `06`, starting OFF | OFF | 1% / 30% | 4000 / 4200 K | `10` |

These are observations under one ambient-light condition, not fixed Auto targets. `06` provides an Auto action without requiring the Auto control bit. That bit is consequently not a reliable indicator that Auto adjustment has occurred. After a `03` Auto request, it remained set through stable readings and a 30-second passive interval; a later manual `03` cleared it. Neither the bit nor stable values establish whether continuous regulation is active or adjustment is complete.

The rear temperature field remained at the manual 4200 K baseline in these Auto observations, including the read after passive listening. Its purpose and the cause of unequal fields remain unknown.

## Other accepted values

| Request | Observation | Remaining limit |
|---|---|---|
| Manual `03` with control `40`, `80`, or `C0` added | Requested brightness applied; corresponding bits remained set in fresh replies | Their functional meaning is unknown |
| Manual `03` with suffix `00`, `01`, or `03` | Requested brightness applied; status replies still used suffix `02` | Tested only under `03`; purpose and other-command behavior remain unknown |
| `05` with timeout index `04` | Fresh replies retained index `04` | Actual inactivity duration and valid upper range were not measured |

These observations do not change the production decoder's supported selection, suffix, timeout, or command set. Its restrictions are documented separately from values accepted by the lamp.

## Queued replies and repeated queries

Across twelve timing sequences, a settings request changed front brightness from 37% to 53%. With rotating query PIDs, replies generally progressed as:

1. Command `04`, old 37% brightness.
2. Command `03`, new 53% brightness.
3. Command `04`, new 53% brightness.

The pattern occurred with requested inter-query delays from zero to 500 ms, repeated twice. For example, with no requested sleep, the first three consumed replies arrived approximately 513, 844, and 1,155 ms after settings-transmit submission. With a requested 500 ms delay, one run consumed them at 1,088, 1,908, and 2,727 ms. This demonstrates request-history effects beyond simply waiting 500 ms; it does not measure ACK queue depth or a universal minimum settling time.

Eight identical status queries using PID `0` repeatedly returned the old `04`/37% payload even though a subsequent rotating-PID read confirmed 53%. Rotating PIDs advanced through the old/status, settings, and fresh/status replies. Keeping PID `0` while alternating an unused query brightness field also advanced the replies after an unsuccessful receive. Changing settings payloads while reusing one PID likewise reached the final requested brightness.

The [BC5602 transport comparison](PROTOCOL.md#replies-polling-and-ordering), reviewed on 2026-10-01, supplies a hardware basis for this interpretation. The lamp's actual queue usage and duplicate behavior under concurrent traffic remain unverified. The evidence supports rotating PIDs and rejecting queued non-status replies; it does not prove freshness under simultaneous controller traffic.

No CRC-valid packets were received during the 30.02-second interval with bridge transmissions disabled after Auto. This only establishes that no unsolicited updates were captured in that interval.

## Timeout sequencing exception

Several requests to shorten timeout index `03` to `01` were not reflected in fresh status despite the probe receiving a matching CRC-valid ACK. Reissuing `05` after a complete readback cycle applied the change, including retries using the same PID as the earlier `05` request.

- Standalone `05` applied the reset in both repeated comparison runs.
- Grouped `03`, then `02`, then `05` with a requested 600 ms pause after each preceding command did not apply the reset in both repeated comparison runs. Retrying after readback applied it.
- Focused checks after `02` also failed with requested 100/600 ms pauses in the tested grouped sequences; requested 1.5/3-second pauses succeeded.
- A `03`-only predecessor succeeded with a requested 600 ms pause. A three-second-pause run did not capture a matching valid ACK and did not apply the reset; a later retry succeeded.

Review of the saved journal on 2026-10-01 found that all five acknowledged failures used a different PID and CRC from the immediately preceding transmitted request. They were not copies of that request under the documented BC5602 duplicate test. This comparison does not identify the lamp's last accepted packet, so duplicate handling involving an earlier packet remains possible.

The missing-ACK case must be distinguished from the acknowledged failures. These limited measurements do not establish a clean delay threshold or a fully characterized `02`-specific requirement. Packet acceptance, duplicate handling, command processing, ACK timing, and bridge/radio scheduling remain possible contributors. The captured device timestamps are needed to compare actual submission intervals. A successful local TX or matching ACK is insufficient proof that the requested timeout was applied.

## Remaining unknowns and cleanup

The remaining gaps include pairing, `00`/`01` side effects, preset storage and power-loss persistence, continuous Auto/completion semantics, physical output for selection `3`, reserved-bit meanings, suffix behavior under other commands, the duration represented by timeout `04`, presence timer/motion behavior, lamp ACK FIFO usage and duplicate/timeout-sequencing behavior, and other channels or hardware revisions.

The raw experiment sources and complete journal are retained locally in `.esphome/captures/protocol-probes-20261001T015319Z/`; the exported evidence contains no credentials. The final script replayed the last controller-captured Favorite (48%, 2700 K, presence enabled, five-minute timeout) and verified the saved live settings: both sections on at 12%, 2700 K, presence enabled, five-minute timeout. The user took over firmware/settings restoration after stopping testing; production-firmware restoration was not performed or verified by the agent for this experiment.
