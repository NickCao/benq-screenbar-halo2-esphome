# Configuration

The root YAML files are alternative board profiles. Both include [`packages/halo2-common.yaml`](../packages/halo2-common.yaml), which loads the local [`halo2` external component](../components/halo2/), configures Wi-Fi/native API/OTA, and declares the entities.

Preserve the directory layout when copying the project into ESPHome Builder. The component directory is self-contained; there are no root-level C++ includes to copy separately.

## Credentials

Copy [`secrets.example.yaml`](../secrets.example.yaml) to `secrets.yaml` and replace every placeholder.

| Secret | Purpose |
|---|---|
| `wifi_ssid`, `wifi_password` | Normal Wi-Fi connection |
| `fallback_password` | Password for the `Halo2 recovery` access point |
| `ota_password` | Native ESPHome OTA authentication |
| `api_encryption_key` | 32-byte base64 key for the encrypted native API; generate with `openssl rand -base64 32` |

There are no web-control credentials. The recovery captive portal is for Wi-Fi provisioning, not lamp control. The native API has `reboot_timeout: 0s`, so losing the HA connection does not cause periodic bridge reboots.

## Component options

The schema is defined in [`components/halo2/__init__.py`](../components/halo2/__init__.py).

| Option | Meaning |
|---|---|
| `radio` | Required: `LR1121` or `BM5602`; set by the board profile |
| `front_light`, `back_light` | Required native light configurations; separate brightness and shared color temperature |
| `ultrasonic` | Required switch enabling the lamp's presence mode; not an occupancy sensor |
| `radio_status` | Required diagnostic text sensor |
| `update_interval` | Radio polling and pending-command processing interval; default `50ms` |
| `auto_discover` | LR1121 only; defaults to true, starts discovery when neither an explicit nor saved link exists |
| `radio_address` | LR1121 only; four bytes in register order, overriding the saved link at boot |
| `radio_channel` | LR1121 only; `5`, `46`, or `75`, corresponding to 2405, 2446, or 2475 MHz; default `5` before discovery/restoration |
| `radio_address_sensor` | Optional LR1121 diagnostic text sensor for address and frequency |
| `discover_button` | Optional LR1121 button that starts a fresh passive scan |
| `frequency_deviation_hz` | LR1121 tuning; default `160000`, allowed `1..170999` |
| `pulse_shape` | LR1121 tuning; default `0x09` (Gaussian BT=0.5); accepts `0x00`, `0x08`, `0x09`, `0x0A`, `0x0B` |

The Waveshare profile includes both discovery entities. Keep its validated RF settings unless investigating a different radio variant. `frequency_deviation_hz` and `pulse_shape` do not tune the BM5602 backend.

Leave the polling interval at 50 ms for normal use. Faster polling increases synchronous radio traffic and reduces time available to networking.

The component currently supports one radio/bridge instance per device and uses the fixed board pins described in [wiring](WIRING.md).

## Light defaults and state

Both lights use zero-length transitions by default and `gamma_correct: 1.0`; other gamma values are rejected because the lamp already accepts brightness percentages. Their restore mode is `RESTORE_DEFAULT_OFF`.

The common package provides initial brightness values of 12% front / 91% back and a shared 3925 K temperature. These are initial defaults, not address-discovery requirements. Saved light preferences take precedence on later boots. Initialization restores the bridge's last known state without transmitting it to the lamp.

The hardware temperature range is 2700–6500 K in 25 K steps. A change through either light is mirrored to the other. Brightness is 1–100% when lit; zero brightness is treated as off. Last useful brightness is retained when turning a section off.

There is no `power:` option or master Power switch. The front/back light states determine global power and the selected lighting mode. The ultrasonic switch displays a normal toggle using the last commanded or received value; this does not add lamp acknowledgements.

## LR1121 manual address

Automatic discovery is the usual setup. If you already know a link, add its address and channel to the Waveshare profile. This example uses the legacy reference address; replace it with your own:

```yaml
halo2:
  radio: LR1121
  auto_discover: false
  radio_address: [0x9C, 0xEA, 0xBB, 0x86]
  radio_channel: 5
```

The address is in **register order**, the same order shown by **Radio address**. Its bytes are reversed for the on-air address. Operate the original controller after configuration so the bridge can learn the current settings and the packet-option byte used by the lamp.

An explicit address takes precedence over preferences at boot. Remove `radio_address` and allow discovery if you want a newly learned link to be reused automatically. With `auto_discover: false` and no explicit or saved link, the implementation falls back to its compile-time reference address; that is not a universal lamp address.

## BM5602 address

The legacy backend uses `RADIO_ADDRESS` and `RADIO_CHANNEL` in [`components/halo2/halo2_protocol.h`](../components/halo2/halo2_protocol.h). The reference values are `9C EA BB 86` in register order and channel `5`. Use values captured from your own controller/lamp pair before compiling.

The YAML discovery/address options are rejected for `BM5602`. Editing the shared defaults also changes the LR1121 fallback, but not an explicit or restored LR1121 link.

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

The radio headers now live under `components/halo2/`; remove old `esphome.includes` entries from customized configurations and copy the complete component directory. The root board profile names are unchanged.
