#pragma once

namespace esphome::button {
class Button {
 public:
  virtual ~Button() = default;

 protected:
  virtual void press_action() = 0;
};
}  // namespace esphome::button
