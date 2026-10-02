#include <cstdlib>
#include <iostream>
#include "lamp_state.h"

using namespace esphome::halo2;

static void check(bool condition, const char *expression, int line) {
  if (!condition) {
    std::cerr << "Line " << line << ": " << expression << '\n';
    std::exit(EXIT_FAILURE);
  }
}
#define CHECK(expression) check((expression), #expression, __LINE__)

static LampState initial_state() {
  LampState state;
  state.power = true;
  state.front_brightness = 40;
  state.back_brightness = 70;
  state.color_temperature = 4000;
  state.ultrasonic_enabled = true;
  state.ultrasonic_timeout = UltrasonicTimeout::MINUTES_10;
  return state;
}

static void settings_are_independent_of_power_and_selection() {
  for (auto selection : {LampSelection::FRONT_ONLY, LampSelection::BACK_ONLY, LampSelection::BOTH}) {
    for (bool power : {false, true}) {
      auto state = initial_state();
      state.selection = selection;
      state.power = power;
      state.set_brightness(Section::FRONT, 55);
      state.set_brightness(Section::BACK, 25);
      CHECK(state.selection == selection);
      CHECK(state.power == power);
      CHECK(state.front_brightness == 55 && state.back_brightness == 25);
      state.set_brightness(Section::FRONT, 0);
      CHECK(state.front_brightness == 55);
      state.set_selection(false, false);
      CHECK(state.selection == selection);
      state.selection = LampSelection::BOTH;
      CHECK(state.front_brightness == 55 && state.back_brightness == 25);
      CHECK(state.power == power);
    }
  }
}

static void brightness_changes_preserve_the_other_section() {
  auto state = initial_state();
  state.selection = LampSelection::FRONT_ONLY;
  state.set_brightness(Section::FRONT, 255);
  CHECK(state.front_brightness == 100);
  CHECK(state.back_brightness == 70);
  CHECK(!state.is_on(Section::BACK));
  state.set_brightness(Section::FRONT, 1);
  CHECK(state.front_brightness == 1);
  CHECK(state.back_brightness == 70);
}

static void master_dimming_preserves_the_profile_and_inactive_levels() {
  for (auto selection : {LampSelection::FRONT_ONLY, LampSelection::BACK_ONLY, LampSelection::BOTH}) {
    for (bool power : {false, true}) {
      auto basis = initial_state();
      basis.selection = selection;
      basis.power = power;
      CHECK(basis.master_brightness() == (selection == LampSelection::FRONT_ONLY ? 40 : 70));
      auto state = basis;
      state.set_master_brightness(0, basis);
      CHECK(state == basis);
      state.set_master_brightness(35, basis);
      CHECK(state.master_brightness() == 35);
      CHECK(state.power == power);
      CHECK(state.selection == selection);
      CHECK(state.front_brightness == (selection == LampSelection::BACK_ONLY ? 40 :
                                        selection == LampSelection::BOTH ? 20 : 35));
      CHECK(state.back_brightness == (selection == LampSelection::FRONT_ONLY ? 70 : 35));
      // A fade always scales against its original profile, avoiding drift
      // from repeatedly rounding the preceding sample.
      state.set_master_brightness(1, basis);
      CHECK(state.master_brightness() == 1);
      state.set_master_brightness(basis.master_brightness(), basis);
      CHECK(state == basis);
      state.set_master_brightness(255, basis);
      CHECK(state.master_brightness() == 100);
    }
  }
}

static void dimming_handles_minimum_levels_and_rounding() {
  for (auto selection : {LampSelection::FRONT_ONLY, LampSelection::BACK_ONLY, LampSelection::BOTH}) {
    for (uint8_t front : {1, 2, 37, 99, 100}) {
      for (uint8_t back : {1, 2, 62, 99, 100}) {
        auto basis = initial_state();
        basis.selection = selection;
        basis.front_brightness = front;
        basis.back_brightness = back;
        auto state = basis;
        for (uint8_t percent : {100, 50, 2, 1}) {
          state.set_master_brightness(percent, basis);
          CHECK(state.master_brightness() == percent);
          CHECK(state.front_brightness >= 1 && state.front_brightness <= 100);
          CHECK(state.back_brightness >= 1 && state.back_brightness <= 100);
          if (!state.selected(Section::FRONT)) CHECK(state.front_brightness == front);
          if (!state.selected(Section::BACK)) CHECK(state.back_brightness == back);
          CHECK(state.power == basis.power);
          CHECK(state.selection == basis.selection);
        }
      }
    }
  }
}

int main() {
  settings_are_independent_of_power_and_selection();
  brightness_changes_preserve_the_other_section();
  master_dimming_preserves_the_profile_and_inactive_levels();
  dimming_handles_minimum_levels_and_rounding();
  std::cout << "Lamp state tests passed\n";
}
