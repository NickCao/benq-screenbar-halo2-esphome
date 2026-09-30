#pragma once

#include "hal.h"

namespace esphome {
struct Application {
  uint32_t get_loop_component_start_time() const { return millis(); }
};
inline Application App;
}  // namespace esphome
