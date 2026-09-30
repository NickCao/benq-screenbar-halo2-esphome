#pragma once

#include <cstddef>
#include <cstdint>
#include "check.h"
#include "esphome/core/log.h"

namespace esphome::spi {
enum SPIBitOrder { BIT_ORDER_MSB_FIRST };
enum SPIClockPolarity { CLOCK_POLARITY_LOW };
enum SPIClockPhase { CLOCK_PHASE_LEADING };
enum SPIDataRate { DATA_RATE_1MHZ };

class SPIComponent {
 public:
  virtual ~SPIComponent() = default;
  virtual void write(const uint8_t *data, size_t size) = 0;
  virtual void read(uint8_t *data, size_t size) = 0;
  bool is_failed() const { return false; }
};

template<SPIBitOrder, SPIClockPolarity, SPIClockPhase, SPIDataRate> class SPIDevice {
 public:
  void set_spi_parent(SPIComponent *parent) { parent_ = parent; }

 protected:
  void spi_setup() { registered_ = true; }
  void spi_teardown() { registered_ = false; }
  bool spi_is_ready() const { return parent_ != nullptr && registered_; }
  void enable() {
    CHECK(!selected_);
    selected_ = true;
  }
  void disable() { selected_ = false; }
  void write_array(const uint8_t *data, size_t size) {
    CHECK(selected_);
    parent_->write(data, size);
  }
  void transfer_array(uint8_t *data, size_t size) {
    CHECK(selected_);
    parent_->read(data, size);
  }

  SPIComponent *parent_{nullptr};
  bool release_device_{false};

 private:
  bool registered_{false}, selected_{false};
};
}  // namespace esphome::spi
