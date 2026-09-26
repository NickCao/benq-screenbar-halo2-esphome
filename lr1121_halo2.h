#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lr1121_radio.h"

namespace lr1121_halo2 {
// Waveshare ESP32-S3-LR1121-HF onboard wiring; GPIO19/20 remain USB.
constexpr gpio_num_t CS = GPIO_NUM_42, SCK = GPIO_NUM_40;
constexpr gpio_num_t MOSI = GPIO_NUM_45, MISO = GPIO_NUM_46;
constexpr gpio_num_t RESET = GPIO_NUM_39, BUSY = GPIO_NUM_41, IRQ = GPIO_NUM_38;

class EspIdfTransport {
 public:
  bool begin() {
    if (device_) return true;
    gpio_config_t pins{};
    pins.pin_bit_mask = (1ULL << BUSY) | (1ULL << IRQ);
    pins.mode = GPIO_MODE_INPUT;
    if (gpio_config(&pins) != ESP_OK) return false;
    pins.pin_bit_mask = 1ULL << RESET;
    pins.mode = GPIO_MODE_OUTPUT;
    if (gpio_set_level(RESET, 1) != ESP_OK || gpio_config(&pins) != ESP_OK) return false;
    spi_bus_config_t bus{};
    bus.mosi_io_num = MOSI;
    bus.miso_io_num = MISO;
    bus.sclk_io_num = SCK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 32;
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED) != ESP_OK) return false;
    spi_device_interface_config_t config{};
    config.clock_speed_hz = 1000000;
    config.mode = 0;
    config.spics_io_num = CS;
    config.queue_size = 1;
    if (spi_bus_add_device(SPI2_HOST, &config, &device_) != ESP_OK) {
      spi_bus_free(SPI2_HOST);
      return false;
    }
    return true;
  }
  void reset() {
    gpio_set_level(RESET, 0);
    delay_ms(1);
    gpio_set_level(RESET, 1);
    delay_ms(10);
  }
  bool busy() const { return gpio_get_level(BUSY); }
  bool irq() const { return gpio_get_level(IRQ); }
  int64_t now_us() const { return esp_timer_get_time(); }
  void delay_us(uint32_t us) { esp_rom_delay_us(us); }
  void delay_ms(uint32_t ms) {
    const TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks ? ticks : 1);
  }
  bool transfer(const uint8_t *tx, uint8_t *rx, size_t size) {
    spi_transaction_t transaction{};
    transaction.length = size * 8;
    transaction.tx_buffer = tx;
    transaction.rx_buffer = rx;
    return device_ && spi_device_polling_transmit(device_, &transaction) == ESP_OK;
  }
 private:
  spi_device_handle_t device_ = nullptr;
};

using halo2_protocol::HaloRxState;
inline EspIdfTransport transport;
inline Radio<EspIdfTransport> radio(transport);
inline uint8_t halo_app_pid = 0, halo_last_pcf = 0;
inline uint16_t halo_last_crc = 0;
inline const char *last_error() { return radio.last_error(); }
inline bool setup(uint32_t deviation_hz = 160000, uint8_t pulse_shape = 0x09,
                  const halo2_protocol::Address &address = halo2_protocol::RADIO_ADDRESS,
                  uint8_t channel = halo2_protocol::RADIO_CHANNEL) {
  return radio.setup(deviation_hz, pulse_shape, address, channel);
}
inline bool poll_halo_receive(HaloRxState &state) { return radio.poll(state); }
inline bool send_halo_state(uint8_t command, bool power, bool pir, bool front, bool back,
                            uint8_t front_brightness, uint8_t back_brightness, uint16_t color_temperature,
                            uint8_t packet_options = 0x01) {
  if (!radio.ready()) return false;
  const auto payload = halo2_protocol::make_payload(command, power, pir, front, back,
                                                   front_brightness, back_brightness, color_temperature, packet_options);
  halo_last_pcf = halo2_protocol::request_pcf(halo_app_pid++);
  const auto frame = halo2_protocol::make_air_frame(halo_last_pcf, payload, radio.address());
  halo_last_crc = halo2_protocol::halo_crc(halo_last_pcf, payload.data(), payload.size(), radio.address(), true);
  return radio.send(frame);
}
}  // namespace lr1121_halo2
