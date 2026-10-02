# Configuration

The Waveshare board profile, [`screenbar-halo2-lr1121.yaml`](../screenbar-halo2-lr1121.yaml), includes [`packages/halo2-common.yaml`](../packages/halo2-common.yaml), which loads the local [`halo2` external component](../components/halo2/), configures Wi-Fi/native API/OTA, and declares the entities.

Preserve the directory layout when copying the project into ESPHome Builder. The component directory is self-contained; there are no root-level C++ includes to copy separately.

## Credentials

Copy [`secrets.example.yaml`](../secrets.example.yaml) to `secrets.yaml` and replace every placeholder.

| Secret | Purpose |
|---|---|
| `wifi_ssid`, `wifi_password` | Normal Wi-Fi connection |
| `fallback_password` | Password for the `Halo2 recovery` access point |
| `api_encryption_key` | 32-byte base64 key shared by the encrypted native API and OTA; generate with `openssl rand -base64 32` |

There are no web-control credentials. The recovery captive portal is for Wi-Fi provisioning, not lamp control. The native API has `reboot_timeout: 0s`, so losing the HA connection does not cause periodic bridge reboots.

Native OTA requires encryption, inheriting `api.encryption.key`. There is no `ota_password` in the normal configuration. Keep the existing API key when upgrading a device already connected to Home Assistant.

## Encrypted OTA migration

ESPHome 2026.9.0 or newer is required; the Podman wrapper pins 2026.9.0. A USB install can enable encrypted OTA directly. After installation, both the firmware and uploader require encrypted native OTA transfers. See [ESPHome's OTA encryption documentation](https://esphome.io/components/ota/esphome/#encryption).

An existing device running older, password-based firmware needs either that USB install or ESPHome's two-step network migration:

1. Temporarily retain the old `password: !secret ota_password` in the common package's OTA block instead of `encryption:`. Keep the API key unchanged, build with ESPHome 2026.9.0, and upload using the old OTA password. This first upload is authenticated but unencrypted. The resulting firmware offers encrypted OTA alongside the old protocol.
2. Restore the committed `encryption:` block, remove the OTA `password:` line, and build/upload again. This upload is encrypted, and the resulting firmware requires encryption. The startup log should report `Encryption: required`.

Keep the old OTA password in your local `secrets.yaml` until migration succeeds; it can then be removed. Do not regenerate the API key during the migration. Losing or changing that key requires a recovery install, such as USB, before encrypted OTA will work again.

The fallback access point's captive portal remains a local recovery path; it is separate from native encrypted OTA on port 3232.

## Component options

The schema is defined in [`components/halo2/__init__.py`](../components/halo2/__init__.py).

| Option | Meaning |
|---|---|
| `spi_id`, `cs_pin` | ESPHome SPI bus reference and required chip-select output pin. The bus must declare both MOSI and MISO. |
| `reset_pin`, `busy_pin`, `irq_pin` | Required internal GPIO pins: reset output, BUSY input, and IRQ input |
| `data_rate`, `spi_mode` | Fixed to the validated `1MHz` and `MODE0`, also used as defaults |
| `light` | Required master light: global power, shared color temperature, and proportional dimming |
| `sections` | Required select with Front, Back, and Both options |
| `front_brightness`, `back_brightness` | Required numbers with range 1–100%, step 1, and SLIDER mode by default; optional `initial_value` defaults to 100 |
| `ultrasonic` | Required select: Disabled, 3 minutes, 5 minutes, 10 minutes, or 15 minutes; a duration enables presence detection with that inactivity timeout |
| `auto_button` | Optional button activating the lamp's automatic brightness adjustment; included in the common package |
| `radio_status` | Required diagnostic text sensor |
| `update_interval` | Receive-buffer checks and pending-command processing interval; default `50ms` |
| `command_debounce` | Minimum gap after a command batch; changes during this interval are combined into the latest state. Default `1s`; `0s` disables it. The first request has no added delay. |
| `status_poll_interval` | Interval between lamp status refresh cycles, default `5s`, minimum `1s` |
| `auto_discover` | Defaults to true, starts discovery when neither an explicit nor saved link exists |
| `radio_address` | Four bytes in register order, overriding the saved link at boot |
| `radio_channel` | `5`, `46`, or `75`, corresponding to 2405, 2446, or 2475 MHz; default `5` before discovery/restoration |
| `radio_address_sensor` | Optional LR1121 diagnostic text sensor for address and frequency |
| `discover_button` | Optional LR1121 button that starts a fresh passive scan |
| `frequency_deviation_hz` | LR1121 tuning; default `160000`, allowed `1..170999` |
| `pulse_shape` | LR1121 tuning; default `0x09` (Gaussian BT=0.5); accepts `0x00`, `0x08`, `0x09`, `0x0A`, `0x0B` |

The Waveshare profile includes both discovery entities. Keep its validated RF settings unless investigating a different radio variant.

Leave `update_interval` at 50 ms for normal use. It controls local processing, not how often status queries are transmitted. `status_poll_interval` controls those queries independently; each cycle normally uses two requests about half a second apart to discard the lamp's old queued reply before reading fresh state. Up to two extra reads drain any additional queued command replies. Replies are checked without blocking ESPHome while waiting. Faster processing or status polling increases radio work.

Completed HA command batches schedule a status refresh after 500 ms. Polling pauses during pending commands, light transitions and discovery. Three consecutive failed cycles produce a **Radio status** warning and retain the last known light states; successful polling clears it.

The first request is handled on the next `update_interval` tick when the radio is available. Requests arriving during the cooldown are combined and sent when it expires; they do not restart the timer. The required settings-plus-power sequence remains one batch. For example:

```yaml
halo2:
  command_debounce: 1s
```

LR1121 reset, BUSY, transmit completion and receive processing run asynchronously. A low-level radio error triggers retries after 1, 2, 4, 8, 16, then 30 seconds, capped at 30 seconds until initialization succeeds. Only the radio resets; the learned address, Wi-Fi and native API are retained. Incomplete commands are discarded and actual lamp status is queried after recovery.

The component currently supports one radio/bridge instance per device. LR1121 wiring is configured in the board YAML. See [wiring](WIRING.md).

## Light defaults and state

The master light has `name: ""` so HA uses the device name once. The common package sets `friendly_name: ScreenBar`, which also preserves the API key of the earlier explicitly named ScreenBar light.

The master light uses zero-length transitions by default and `gamma_correct: 1.0`; other gamma values are rejected because the lamp already accepts brightness percentages. Its restore mode is `RESTORE_DEFAULT_OFF`.

The common package provides initial brightness values of 12% front / 91% back and a shared 3925 K temperature. Saved master power/temperature and lighting mode are restored at startup, with the configured section brightness defaults used until readback. Initialization sends no settings or power commands. LR1121 queries the lamp, and local controls are accepted after the first valid lamp/controller state. Both section levels and all other settings are then reconciled with received state.

**Lighting mode** directly represents the lamp's Front, Back, or Both selection, independently of global power. **Front brightness** and **Back brightness** show the stored 1–100% levels, including while a section is unselected or the lamp is off. The lamp applies brightness changes only to selected sections: select a section before editing its level; an inactive section's slider returns to its stored value. Setting changes while globally off keep the lamp off.

The master brightness is the highest selected level. Its native brightness control scales selected sections proportionally; front 30% / back 80% gives master 80%, and changing master to 40% produces 15% / 40%. Unselected levels are retained. Outgoing percentages are rounded to integers with a 1% minimum; a selected section never becomes unselected through dimming. The native HA brightness action turns the master on. Controller changes and fresh lamp status establish the actual levels used for the next adjustment.

Master OFF, including a zero master brightness request, retains the selection and both levels. Explicit OFF transitions defer power-off until completion and preserve the stored wake settings throughout; they do not send intermediate dimming levels. A subsequent plain ON restores the retained profile. BenQ documents a 30-second presence pause after manual power-off; the lamp owns that pause, and the bridge does not add a timer. [BenQ presence FAQ](https://www.benq.com/th-th/support/downloads-faq/faq/product/application/e-reading-lamp-faq-kn-00051.html)

The master light owns shared color temperature, sent as 2700–6500 K in 25 K steps. Both payload temperature fields carry the same value. The section selector and brightness numbers finish an active master transition at its target before applying their setting, so the previous fade cannot overwrite the new profile.

The **Ultrasonic sensor** dropdown combines sensor enable and inactivity timeout into one control. Disabled turns off presence detection while retaining the lamp's timeout; choosing 3, 5, 10, or 15 minutes enables detection with that duration. The timer runs in the lamp. Polling and accepted controller snapshots update the dropdown; it does not report occupancy. On the wire, timeout value zero means three minutes, and disabling uses a separate bit; see the [protocol reference](PROTOCOL.md#ultrasonic-timeout).

Press **Auto brightness** with the lamp on to activate its automatic adjustment. It is a button, not a separate on/off entity. Ordinary manual brightness commands resume manual control. On LR1121, the resulting brightness and temperature are read back through status polling.

## LR1121 manual address

Automatic discovery is the usual setup. If you already know a link, add its address and channel to the Waveshare profile. This example uses the legacy reference address; replace it with your own:

```yaml
halo2:
  auto_discover: false
  radio_address: [0x9C, 0xEA, 0xBB, 0x86]
  radio_channel: 5
```

The address is in **register order**, the same order shown by **Radio address**. Its bytes are reversed for the on-air address. With the correct address and channel, polling reads the current settings, including presence enable and timeout; accepted controller traffic can also supply them.

An explicit address takes precedence over preferences at boot. Remove `radio_address` and allow discovery if you want a newly learned link to be reused automatically. With `auto_discover: false` and no explicit or saved link, the implementation falls back to its compile-time reference address; that is not a universal lamp address.

## Optional dashboard

The firmware advertises front/back brightness as number entities in slider mode. Dashboard cards and their features are configured in HA; firmware updates do not install a dashboard. The supplied dashboard uses Tile cards with `numeric-input` features and `style: slider` to keep both brightness controls visible. To configure an existing dashboard through the UI, add a Tile card for each brightness number, then add the Numeric input feature with Slider style. [HA card features](https://www.home-assistant.io/dashboards/features/#numeric-input)

The native API integration is sufficient for control. To install the supplied YAML dashboard, copy [`home-assistant/dashboard.yaml`](../home-assistant/dashboard.yaml) to your HA configuration as `dashboards/screenbar_halo2.yaml` and register it in `configuration.yaml`:

```yaml
lovelace:
  dashboards:
    screenbar-yaml:
      mode: yaml
      title: ScreenBar
      icon: mdi:monitor-shimmer
      show_in_sidebar: true
      filename: dashboards/screenbar_halo2.yaml
```

Merge this into an existing `lovelace:` section if present. The dashboard uses entity IDs for a new installation with the default ScreenBar device name. Existing installations retain their entity IDs; adjust the dashboard to match yours. There is no HA package or REST polling automation to copy.

Use the master ScreenBar light for HA light controls and automations. Its ON action retains the selected lighting mode; use Lighting mode to choose sections explicitly.

## Updating older installations

HA refreshes the native API entity list on reconnect. The previous Front lamp and Back lamp entities are replaced by ScreenBar, Lighting mode, and the two brightness numbers. Update dashboard cards and automations to the new entities, remove the old Light group helper, and remove retired entities once HA marks them unavailable. In customized YAML, replace `front_light` / `back_light` with `light`, `sections`, `front_brightness`, and `back_brightness`, following the common package. Also remove references to the earlier master Power switch or REST helpers.

The ultrasonic entity is now a select instead of a switch. Update dashboard and automation references from `switch.…_ultrasonic_sensor` to `select.…_ultrasonic_sensor`, using one of the five option names above. The previous switch may remain as an unavailable entity in HA and can be removed. Customized `ultrasonic:` configurations must remove switch-only options such as `restore_mode` or `inverted`.

The radio headers live under `components/halo2/`; remove old `esphome.includes` entries from customized configurations and copy the complete component directory. The Waveshare profile remains `screenbar-halo2-lr1121.yaml`.

Customized configurations also need the `spi:` bus and `spi_id`, `cs_pin`, `reset_pin`, `busy_pin`, and `irq_pin` settings from the current Waveshare profile. Pin assignments are no longer compiled into the LR1121 transport. Remove `halo2.radio`; the component now supports LR1121 exclusively.
