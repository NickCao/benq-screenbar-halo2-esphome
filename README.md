# BenQ ScreenBar HALO 2 · ESPHome radio bridge

[![Validate](https://github.com/Termina1/benq-screenbar-halo2-esphome/actions/workflows/validate.yml/badge.svg)](https://github.com/Termina1/benq-screenbar-halo2-esphome/actions/workflows/validate.yml)

Control a **BenQ ScreenBar HALO 2** through Home Assistant's encrypted ESPHome native API. The bridge exposes **Front lamp** and **Back lamp** as lights with separate power and brightness controls and a shared color temperature. It also supports Auto brightness, ultrasonic presence mode, and commands from the original controller. The LR1121 profile polls the lamp to keep HA updated after automatic changes.

The primary hardware is the **Waveshare ESP32-S3-LR1121-HF**, using its onboard radio at 2.4 GHz in GFSK mode. The older **M5Stack ATOM Lite + BM5602** implementation is retained as an alternative.

## Hardware profiles

| Configuration | Hardware | Address setup |
|---|---|---|
| [`screenbar-halo2-lr1121.yaml`](screenbar-halo2-lr1121.yaml) | Waveshare ESP32-S3-LR1121-HF, 4 MB flash / 2 MB PSRAM | Automatic discovery from the original controller, or an explicit address |
| [`screenbar-halo2.yaml`](screenbar-halo2.yaml) | M5Stack ATOM Lite with an external BM5602 | Compile-time address; requires seven-wire wiring including TBCLK |

Choose one profile for your bridge. Both use the device name `screenbar-halo2`; rename it if deploying more than one bridge. See [wiring and antenna connections](docs/WIRING.md) before powering the hardware.

## Setup with Podman

The commands below use Linux, Bash, and Podman. [`scripts/esphome`](scripts/esphome) runs **ESPHome 2026.9.0** and caches its tools under the ignored `.esphome/cache/` directory. ESPHome 2026.9.0 or newer is required for encrypted OTA. No host ESPHome installation is required.

### 1. Configure credentials

Clone or download the whole repository, preserving the `components/` and `packages/` directories alongside the board YAML files. From the repository root:

```bash
cp secrets.example.yaml secrets.yaml
chmod 600 secrets.yaml
openssl rand -base64 32
```

Edit `secrets.yaml`: supply your Wi-Fi SSID and password, a separate recovery-AP password, and the generated API encryption key. Native API and OTA both use that key for encryption and authentication; no separate OTA password is needed. Keep this file private; it is ignored by Git. Build artifacts contain these credentials too and should not be published.

### 2. Compile and flash over USB

For the Waveshare board, connect its USB adapter and use the appropriate serial device on your host:

```bash
ESPHOME_SERIAL_DEVICE=/dev/ttyACM0 ./scripts/esphome run screenbar-halo2-lr1121.yaml --device /dev/ttyACM0
```

The wrapper passes only the selected serial device into Podman. Your host user must have permission to access it. Rootless USB passthrough uses `keep-groups`, which requires Podman's `crun` runtime.

For the ATOM Lite, use `screenbar-halo2.yaml` and its serial device instead, after completing the BM5602 wiring and setting its radio address.

To compile without flashing:

```bash
./scripts/esphome compile screenbar-halo2-lr1121.yaml
```

### 3. Add the bridge to Home Assistant

Accept the discovered ESPHome device, or add **Settings → Devices & services → Add integration → ESPHome** manually. Use `screenbar-halo2.local` (or its IP), port `6053`, and the API encryption key from `secrets.yaml`.

The integration uses the native API directly. There is no REST package, MQTT configuration, or lamp-control web server to install. The fallback access point is for Wi-Fi recovery.

### 4. Discover the lamp address (LR1121)

With no explicit or previously saved radio address, discovery starts automatically. Keep the original controller near the board and repeatedly adjust brightness until **Radio status** reports discovery completed and **Radio address** shows an address and frequency. No specific brightness, temperature, or front/back selection is required.

The bridge scans 2405, 2446, and 2475 MHz. It saves the link after three matching CRC-valid captures, so allow it time to scan while continuing to operate the controller. Discovery is passive: lamp commands from HA are blocked during the scan.

The saved link is reused after reboot. Use **Discover lamp address** to learn another controller/lamp pair. An explicit YAML address takes precedence at the next boot; remove it to use the learned link. See [configuration](docs/CONFIGURATION.md) for manual addressing.

## Home Assistant controls

| Entity | Behavior |
|---|---|
| Front lamp | Front on/off, brightness, and shared color temperature |
| Back lamp | Back on/off, brightness, and shared color temperature |
| Auto brightness | Activates the lamp's automatic brightness adjustment |
| Ultrasonic sensor | Normal toggle for the lamp's automatic presence mode; it does not report occupancy |
| Radio status | Initialization, discovery, controller reception, lamp polling, and command/error status |
| Radio address | Learned/configured address and frequency; LR1121 only |
| Discover lamp address | Starts a new passive scan; LR1121 only |
| Resend current state | Reapplies the bridge's current settings and power state |
| Restart | Restarts the bridge |

There is no separate master Power entity. Turning off the last active section sends the lamp's global OFF command; turning either section on from fully off applies the settings and sends global ON.

The first command is sent without a debounce delay. For one second after a command batch finishes, further changes are combined into a single final update. HA reflects requests immediately. Set `halo2.command_debounce` to adjust this interval.

Brightness is independent for each section. Color temperature is shared by the lamp, so changing it on either entity updates both. The range is 2700–6500 K in 25 K steps. [BenQ user guide, English page 5](https://esupportdownload.benq.com/esupport/E-READING%20LAMP/UserManual/ScreenBar%20Halo%202/ScreenBar%20Halo%202_UM_DE_EN_ES_FR_IT_JA_NL_SV_ZH-TW_250627174322.pdf)

HA's normal “all lights” controls operate both sections. For one dedicated ScreenBar control, optionally create an HA **Light group** containing Front lamp and Back lamp. Its default state is on if either member is on; group ON turns both sections on. The group is configured in HA, not created by this firmware. [HA light groups](https://www.home-assistant.io/integrations/group/)

[`home-assistant/dashboard.yaml`](home-assistant/dashboard.yaml) provides an optional native-entity dashboard. Adapt its entity IDs if you renamed the device or entities. See [dashboard setup](docs/CONFIGURATION.md#optional-dashboard).

## State synchronization and limits

HA commands update the bridge's state optimistically. CRC-valid requests heard from the original controller update the same state and are published to HA without echoing another radio command. A later received full-state request can recover missed controller changes.

The **LR1121 profile queries the lamp every five seconds**, so presence-triggered on/off, Auto brightness adjustments, and missed controller changes are reflected in HA after the next successful poll. Each cycle refreshes the lamp's queued status, waits half a second, and reads it back. The first reply can contain old state and is discarded, preventing it from undoing a recent HA command. Local commands trigger an earlier refresh.

After three failed polling cycles, **Radio status** reports that lamp status is unavailable; the lights retain their last known state. A successful reply restores synchronization. **Command sent** means local radio transmission completed; **Lamp status received** indicates an actual status reply. See [polling configuration](docs/CONFIGURATION.md#component-options).

LR1121 hardware errors trigger automatic radio reinitialization, with retries from one to thirty seconds apart. Wi-Fi and the native API remain running. Recovery restores the radio link and reads the lamp's state without replaying interrupted commands.

The legacy **BM5602 profile only listens to controller requests**. It does not poll the lamp, so autonomous changes or missed traffic can leave its HA state stale. **Resend current state** reapplies HA's settings to the lamp on either profile; it is not a status query.

## Updates and logs

OTA requires encryption and reuses the existing native API key. When upgrading from this project's older password-based firmware, install once over USB using the command above, or follow the [two-step OTA migration](docs/CONFIGURATION.md#encrypted-ota-migration). Simply uploading the new configuration to old firmware over the network will be refused because the old firmware cannot negotiate encryption.

Use the same board profile you initially flashed:

```bash
./scripts/esphome run screenbar-halo2-lr1121.yaml --device screenbar-halo2.local
./scripts/esphome logs screenbar-halo2-lr1121.yaml --device screenbar-halo2.local
```

`run` compiles the selected profile before uploading. Each profile has a separate build directory; both target the same default hostname.

## Repository layout

| Path | Purpose |
|---|---|
| [`components/halo2/`](components/halo2/) | Self-contained ESPHome external component, shared protocol, and radio backends |
| [`packages/halo2-common.yaml`](packages/halo2-common.yaml) | Networking, native API, entities, and common defaults |
| `screenbar-halo2*.yaml` | Board-specific entry points |
| [`scripts/esphome`](scripts/esphome) | Podman wrapper used locally and by CI |
| [`secrets.example.yaml`](secrets.example.yaml) | Credential template |
| [`home-assistant/dashboard.yaml`](home-assistant/dashboard.yaml) | Optional HA dashboard |
| [`docs/CONFIGURATION.md`](docs/CONFIGURATION.md) | Component options, manual addressing, and dashboard setup |
| [`docs/WIRING.md`](docs/WIRING.md) | Waveshare antennas/pins and ATOM Lite wiring |
| [`docs/IMPLEMENTATION_NOTES.md`](docs/IMPLEMENTATION_NOTES.md) | State handling, discovery, and radio frame formats |
| [`tests/test_bm5602_crc.py`](tests/test_bm5602_crc.py) | Existing legacy BM5602 CRC vectors |
| [`.github/workflows/validate.yml`](.github/workflows/validate.yml) | CRC checks and Podman compilation for both boards |

All C++ sources needed by `halo2` live in its component directory and are copied by ESPHome automatically. Board YAML files do not need `esphome.includes`. This follows ESPHome's [external-component layout](https://esphome.io/components/external_components/).

## Development

```bash
python3 -m unittest discover -s tests -v
./scripts/esphome compile screenbar-halo2-lr1121.yaml
./scripts/esphome compile screenbar-halo2.yaml
```

The Python vectors cover the legacy BM5602 CRC representation; they do not exercise LR1121 discovery or hardware. CI uses dummy credentials and compiles both profiles. The wrapper defaults to four compiler processes; override `ESPHOME_DEFAULT_COMPILE_PROCESS_LIMIT` if needed. `ESPHOME_IMAGE` can select another image for compatibility checks.

## Project notes

Radio interoperability research was informed by public BM5602 examples and [kuzmin-no/BenQ_ScreenBar_HALO_2_HA_integration](https://github.com/kuzmin-no/BenQ_ScreenBar_HALO_2_HA_integration). The current bridge provides native ESPHome lights and an LR1121 implementation alongside the legacy BM5602 backend.

This is an unofficial community project, not affiliated with or endorsed by BenQ. BenQ and ScreenBar are trademarks of their respective owner.

MIT license. See [LICENSE](LICENSE).
