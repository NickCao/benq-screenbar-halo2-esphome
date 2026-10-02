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

static void restored_settings_require_received_state() {
  LampStateModel model;
  auto saved = initial_state();
  saved.power = false;
  saved.selection = LampSelection::BACK_ONLY;
  model.restore(saved);
  CHECK(model.requested() == saved);
  CHECK(!model.initialized());

  auto local = saved;
  local.set_brightness(Section::FRONT, 55);
  model.request(local);
  CHECK(!model.initialized());

  // Hearing the controller establishes settings for further commands, but
  // does not prove that the lamp applied them.
  CHECK(model.receive_request(saved));
  CHECK(model.initialized());
  CHECK(model.requested() == saved);
  CHECK(!model.receive_request(saved));
}

static void controller_snapshots_replace_local_requests() {
  LampStateModel model;
  const auto confirmed = initial_state();
  CHECK(model.receive_status(confirmed));

  auto local = confirmed;
  local.selection = LampSelection::BACK_ONLY;
  model.request(local);
  CHECK(model.requested().selection == LampSelection::BACK_ONLY);
  CHECK(model.requested() == local);

  auto controller = local;
  controller.back_brightness = 25;
  CHECK(model.receive_request(controller));
  CHECK(model.requested() == controller);
}

static void matching_status_only_republishes_when_establishing_a_baseline() {
  LampStateModel model;
  const auto controller = initial_state();
  CHECK(model.receive_request(controller));
  CHECK(!model.receive_status(controller));
  CHECK(model.requested() == controller);

  model.restore(controller);
  CHECK(model.receive_status(controller));
  CHECK(model.initialized());
  CHECK(!model.receive_status(controller));
}

static void lamp_readback_reconciles_a_failed_request() {
  LampStateModel model;
  const auto actual = initial_state();
  model.receive_status(actual);
  auto desired = actual;
  desired.power = false;
  desired.ultrasonic_enabled = false;
  model.request(desired);

  CHECK(model.receive_status(actual));
  CHECK(model.requested().power);
  CHECK(model.requested().ultrasonic_enabled);
  CHECK(model.requested() == actual);
  CHECK(!model.receive_status(actual));
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

static void fresh_readback_reconciles_inactive_stored_brightness() {
  for (auto section : {Section::FRONT, Section::BACK}) {
    LampStateModel model;
    auto actual = initial_state();
    actual.selection = section == Section::FRONT ? LampSelection::BACK_ONLY : LampSelection::FRONT_ONLY;
    model.receive_status(actual);
    auto requested = actual;
    requested.set_brightness(section, 80);
    model.request(requested);
    actual.set_brightness(section, 1);
    CHECK(model.receive_status(actual));
    CHECK(model.requested() == actual);
    CHECK(!model.receive_status(actual));
    actual.set_brightness(section, 25);
    CHECK(model.receive_request(actual));
    CHECK(model.requested() == actual);
    model.invalidate();
    actual.set_brightness(section, 30);
    CHECK(model.receive_status(actual));
    CHECK(model.requested() == actual);
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

static void recovery_retains_requests_but_requires_a_new_baseline() {
  LampStateModel model;
  const auto confirmed = initial_state();
  model.receive_status(confirmed);
  auto local = confirmed;
  local.power = false;
  model.request(local);

  model.invalidate();
  CHECK(!model.initialized());
  CHECK(model.requested() == local);
  model.request(local);
  CHECK(!model.initialized());
  CHECK(model.receive_status(confirmed));
  CHECK(model.initialized());
  CHECK(model.requested() == confirmed);

  model.restore(local);
  CHECK(!model.initialized());
  CHECK(model.requested() == local);
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
  restored_settings_require_received_state();
  controller_snapshots_replace_local_requests();
  matching_status_only_republishes_when_establishing_a_baseline();
  lamp_readback_reconciles_a_failed_request();
  settings_are_independent_of_power_and_selection();
  fresh_readback_reconciles_inactive_stored_brightness();
  brightness_changes_preserve_the_other_section();
  recovery_retains_requests_but_requires_a_new_baseline();
  master_dimming_preserves_the_profile_and_inactive_levels();
  dimming_handles_minimum_levels_and_rounding();
  std::cout << "Lamp state tests passed\n";
}
