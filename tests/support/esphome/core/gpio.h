#pragma once

#include <functional>

namespace esphome {
class InternalGPIOPin {
 public:
  void setup() {}
  bool digital_read() const { return read ? read() : value; }
  void digital_write(bool level) {
    value = level;
    if (write) write(level);
  }

  bool value{false};
  std::function<bool()> read;
  std::function<void(bool)> write;
};
}  // namespace esphome
