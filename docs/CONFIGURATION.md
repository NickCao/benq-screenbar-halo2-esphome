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
| `front_light`, `back_light` | Required native light configurations; separate brightness and shared color temperature |
| `ultrasonic` | Required switch enabling the lamp's presence mode; not an occupancy sensor |
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

Both lights use zero-length transitions by default and `gamma_correct: 1.0`; other gamma values are rejected because the lamp already accepts brightness percentages. Their restore mode is `RESTORE_DEFAULT_OFF`.

The common package provides initial brightness values of 12% front / 91% back and a shared 3925 K temperature. These are initial defaults, not address-discovery requirements. Saved light preferences take precedence on later boots. Initialization restores the bridge's last known state without sending settings or power commands. LR1121 then queries the lamp to obtain its actual state.

The hardware temperature range is 2700–6500 K in 25 K steps. A change through either light is mirrored to the other. Brightness is 1–100% when lit; zero brightness is treated as off. Last useful brightness is retained when turning a section off.

There is no `power:` option or master Power switch. The front/back light states determine global power and the selected lighting mode. The ultrasonic switch displays a normal toggle using the last commanded or received value. LR1121 polling updates that value from the lamp as well.

Press **Auto brightness** with the lamp on to activate its automatic adjustment. It is a button, not a separate on/off entity. Ordinary manual brightness commands resume manual control. On LR1121, the resulting brightness and temperature are read back through status polling.

## LR1121 manual address

Automatic discovery is the usual setup. If you already know a link, add its address and channel to the Waveshare profile. This example uses the legacy reference address; replace it with your own:

```yaml
halo2:
  auto_discover: false
  radio_address: [0x9C, 0xEA, 0xBB, 0x86]
  radio_channel: 5
```

The address is in **register order**, the same order shown by **Radio address**. Its bytes are reversed for the on-air address. Operate the original controller after configuration so the bridge can learn the current settings and the packet-option byte used by the lamp.

An explicit address takes precedence over preferences at boot. Remove `radio_address` and allow discovery if you want a newly learned link to be reused automatically. With `auto_discover: false` and no explicit or saved link, the implementation falls back to its compile-time reference address; that is not a universal lamp address.

## Optional dashboard

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

Merge this into an existing `lovelace:` section if present. Adjust the dashboard entity IDs to the ones in your HA installation. There is no HA package or REST polling automation to copy.

For a combined control, create a Light group helper containing Front lamp and Back lamp. Group ON enables both sections; it does not restore a previous front-only/back-only selection.

## Updating older installations

Keep the device and light names to retain their HA identities. HA refreshes the native API entity list on reconnect. Remove manually configured dashboard cards or automations that reference the retired master Power switch or old REST helpers.

The radio headers live under `components/halo2/`; remove old `esphome.includes` entries from customized configurations and copy the complete component directory. The Waveshare profile remains `screenbar-halo2-lr1121.yaml`.

Customized configurations also need the `spi:` bus and `spi_id`, `cs_pin`, `reset_pin`, `busy_pin`, and `irq_pin` settings from the current Waveshare profile. Pin assignments are no longer compiled into the LR1121 transport. Remove `halo2.radio`; the component now supports LR1121 exclusively.
