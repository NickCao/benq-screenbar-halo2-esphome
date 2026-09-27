# Hardware and wiring

## Waveshare ESP32-S3-LR1121-HF

Use [`screenbar-halo2-lr1121.yaml`](../screenbar-halo2-lr1121.yaml). The ESP32-S3 and LR1121 are already connected on the board; no extra SPI wiring is needed. The configuration targets 4 MB flash and 2 MB quad PSRAM.

Connect the supplied USB adapter through its FFC cable with power disconnected. The firmware uses the ESP32-S3 USB Serial/JTAG interface for flashing and serial logs; GPIO19 and GPIO20 remain available to USB.

| LR1121 signal | ESP32-S3 GPIO |
|---|---:|
| CS | 42 |
| SCK | 40 |
| MOSI | 45 |
| MISO | 46 |
| RESET | 39 |
| BUSY | 41 |
| IRQ / DIO9 | 38 |

These pins are fixed in [`lr1121_halo2.h`](../components/halo2/lr1121_halo2.h). The radio uses SPI2 at 1 MHz and the board's 3.0 V TCXO. See the [Waveshare board documentation](https://docs.waveshare.com/ESP32-S3-LR1121-XF) and [HF schematic](https://github.com/waveshareteam/ESP32-S3-LR1121-XF/blob/main/hardware/schematics/ESP32-S3-LR1121-HF.pdf).

### Antennas

The board has three separate antenna connections. Identify them by function/label, not just by the antenna's appearance.

| Connection | Use in this firmware |
|---|---|
| ESP32 Wi-Fi antenna, beside the FFC connector | Required for network access and HA |
| Connector labelled **2.4G** | Required for LR1121 communication with the lamp and original controller |
| Connector labelled **LoRa** | Sub-GHz path; unused by this firmware |

The LR1121 is active in **2.4 GHz GFSK mode**. The lamp protocol does not use LoRa modulation. Discovery scans 2405, 2446, and 2475 MHz; normal operation uses the learned/configured channel.

The sub-GHz transmitter is not selected. The firmware also holds the board's RTC6603SP switch on its inactive receive branch, keeping the sub-GHz transmit branch isolated from the LoRa connector during standby, reception, and transmission. DIO5 stays high and DIO6 low. The 2.4 GHz RFIO_HF connection bypasses this switch.

This is RF-path isolation, not a separate hardware power cutoff. The [RTC6603SP datasheet](https://files.waveshare.com/upload/c/c6/Datasheet-RTC6603SP-RichWave.pdf) specifies two routing states, with no documented both-off state.

Based on this routing and the firmware's exclusive use of the HF radio, the sub-GHz **LoRa** antenna can be left disconnected with this firmware. Disconnect power before handling antenna connectors. Keep both the Wi-Fi and 2.4G antennas attached. Reattach the sub-GHz antenna before installing firmware that uses that band.

## M5Stack ATOM Lite + BM5602

Use [`screenbar-halo2.yaml`](../screenbar-halo2.yaml). This legacy backend requires seven wires, including the clock signal needed for direct transmission.

![BM5602 wiring](images/bm5602-wiring.png)

![M5Stack ATOM Lite wiring](images/atom-lite-wiring.png)

| BM5602 | Pin | M5Stack ATOM Lite |
|---|---:|---|
| GND | 1 | GND |
| 3V3 | 2 | 3V3 |
| CSN | 4 | GPIO22 |
| SCK | 5 | GPIO23 |
| GIO2 | 6 | GPIO33 |
| SDIO | 7 | GPIO19 |
| GIO3 / TBCLK | 8 | GPIO25 |

Pins 3 and 9 are not connected. Supply voltage is **3.3 V**. Power off before soldering, particularly when attaching the wire to BM5602 pin 8.

GIO2 normally serves as SPI MISO and becomes the direct transmit-data input during transmission. GIO3 supplies TBCLK, which clocks each transmitted bit. Omitting TBCLK prevents this backend from transmitting correctly.

The BM5602 address and channel are compile-time constants; see [configuration](CONFIGURATION.md#bm5602-address). Automatic address discovery is implemented only for LR1121.
