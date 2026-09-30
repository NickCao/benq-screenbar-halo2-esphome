#pragma once

#include "esphome/core/hal.h"

inline int64_t esp_timer_get_time() { return halo2_test::time_us; }
