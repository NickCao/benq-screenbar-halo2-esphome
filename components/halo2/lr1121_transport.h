#pragma once

#include <cstring>
#include "esphome/components/spi/spi.h"
#include "esphome/core/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

namespace esphome::halo2 {

class LR1121Transport : public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW,
                                              spi::CLOCK_PHASE_LEADING, spi::DATA_RATE_1MHZ> {
 public:
  void set_reset_pin(esphome::InternalGPIOPin *pin) { reset_pin_ = pin; }
  void set_busy_pin(esphome::InternalGPIOPin *pin) { busy_pin_ = pin; }
  void set_irq_pin(esphome::InternalGPIOPin *pin) { irq_pin_ = pin; }

  bool begin() {
    if (parent_ == nullptr || parent_->is_failed() || cs_ == nullptr ||
        reset_pin_ == nullptr || busy_pin_ == nullptr || irq_pin_ == nullptr) return false;
    if (!pins_initialized_) {
      busy_pin_->setup();
      irq_pin_->setup();
      reset_pin_->digital_write(true);
      reset_pin_->setup();
      pins_initialized_ = true;
    }
    // Recovery resets the radio without reinitializing or freeing the shared bus.
    if (!registered_) {
      spi_setup();
      registered_ = true;
    }
    if (release_device_ || spi_is_ready()) return true;
    spi_teardown();
    registered_ = false;
    return false;
  }
  bool set_reset(bool high) {
    reset_pin_->digital_write(high);
    return true;
  }
  bool busy() const { return busy_pin_->digital_read(); }
  bool irq() const { return irq_pin_->digital_read(); }
  int64_t now_us() const { return esp_timer_get_time(); }
  void delay_us(uint32_t us) { esp_rom_delay_us(us); }
  bool transfer(const uint8_t *tx, uint8_t *rx, size_t size) {
    if (!registered_ || size > 32) return false;
    enable();
    if (!spi_is_ready()) {
      disable();
      return false;
    }
    if (rx != nullptr) {
      std::memcpy(rx, tx, size);
      transfer_array(rx, size);
    } else {
      write_array(tx, size);
    }
    // Each command/response releases CS and the bus before waiting for BUSY.
    disable();
    // ESPHome logs SPI transfer errors; the radio also checks responses and
    // BUSY/TX deadlines because the SPI abstraction has no transfer result.
    return true;
  }
  void dump_config() {
    static const char *const TAG = "halo2";
    LOG_SPI_DEVICE(this);
    LOG_PIN("  Reset Pin: ", reset_pin_);
    LOG_PIN("  Busy Pin: ", busy_pin_);
    LOG_PIN("  IRQ Pin: ", irq_pin_);
  }
 private:
  esphome::InternalGPIOPin *reset_pin_{nullptr};
  esphome::InternalGPIOPin *busy_pin_{nullptr};
  esphome::InternalGPIOPin *irq_pin_{nullptr};
  bool pins_initialized_{false};
  bool registered_{false};
};

}  // namespace esphome::halo2
