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
  CHECK(!model.observed());

  auto local = saved;
  local.set_light(Section::FRONT, true, 55);
  model.request(local);
  CHECK(!model.initialized());
  CHECK(!model.observed());

  // Hearing the controller establishes settings for further commands, but
  // does not prove that the lamp applied them.
  CHECK(model.receive_request(saved));
  CHECK(model.initialized());
  CHECK(model.requested() == saved);
  CHECK(!model.observed());
  CHECK(!model.receive_request(saved));
}

static void requests_do_not_mutate_confirmed_settings() {
  LampStateModel model;
  const auto confirmed = initial_state();
  CHECK(model.observe(confirmed));

  auto local = confirmed;
  local.set_light(Section::FRONT, false, 0);
  model.request(local);
  CHECK(model.requested().selection == LampSelection::BACK_ONLY);
  CHECK(model.observed() == confirmed);

  auto controller = local;
  controller.back_brightness = 25;
  CHECK(model.receive_request(controller));
  CHECK(model.requested().back_brightness == 25);
  CHECK(model.observed() == confirmed);

  // Confirmation of an optimistic update still records an observation,
  // even though the UI no longer needs to change.
  CHECK(!model.observe(controller));
  CHECK(model.observed() == controller);
}

static void first_confirmation_records_unchanged_settings() {
  LampStateModel model;
  const auto controller = initial_state();
  CHECK(model.receive_request(controller));
  CHECK(!model.observed());
  CHECK(!model.observe(controller));
  CHECK(model.observed() == controller);
}

static void lamp_readback_reconciles_a_failed_request() {
  LampStateModel model;
  const auto actual = initial_state();
  model.observe(actual);
  auto desired = actual;
  desired.power = false;
  desired.ultrasonic_enabled = false;
  model.request(desired);

  CHECK(model.observe(actual));
  CHECK(model.requested().power);
  CHECK(model.requested().ultrasonic_enabled);
  CHECK(model.observed() == actual);
  CHECK(!model.observe(actual));
}

static void zero_brightness_preserves_levels_and_selection() {
  auto state = initial_state();
  state.set_light(Section::FRONT, true, 0);
  CHECK(!state.is_on(Section::FRONT));
  CHECK(state.is_on(Section::BACK));
  CHECK(state.front_brightness == 40);
  CHECK(state.back_brightness == 70);

  state.set_light(Section::BACK, false, 0);
  CHECK(!state.power);
  CHECK(state.selection == LampSelection::BACK_ONLY);
  CHECK(state.back_brightness == 70);
  state.set_selection(false, false);
  CHECK(state.selection == LampSelection::BACK_ONLY);

  state.set_light(Section::FRONT, true, state.brightness(Section::FRONT));
  CHECK(state.power);
  CHECK(state.selection == LampSelection::FRONT_ONLY);
  CHECK(state.front_brightness == 40);
  CHECK(state.back_brightness == 70);
}

static void inactive_brightness_intent_survives_readback_until_the_next_on() {
  for (auto section : {Section::FRONT, Section::BACK}) {
    LampStateModel model;
    const auto baseline = initial_state();
    model.observe(baseline);
    auto requested = baseline;
    requested.set_light(section, false, baseline.brightness(section));
    model.request(requested);
    auto actual = requested;
    actual.set_brightness(section, 1);  // Last transmitted fade sample.
    CHECK(!model.observe(actual));
    CHECK(model.observed() == actual);
    CHECK(model.requested() == requested);

    requested.set_light(section, true, requested.brightness(section));
    model.request(requested);
    actual = requested;
    actual.set_brightness(section, 10);  // A selected section must reconcile.
    CHECK(model.observe(actual));
    CHECK(model.requested() == actual);

    actual.set_light(section, false, 25);
    CHECK(model.receive_request(actual));  // Trust the controller snapshot.
    CHECK(model.requested() == actual);
    model.invalidate();
    actual.set_brightness(section, 30);
    CHECK(model.observe(actual));  // Recovery establishes a fresh baseline.
    CHECK(model.requested() == actual);
  }
}

static void grouped_commands_work_in_either_order() {
  for (auto selection : {LampSelection::FRONT_ONLY, LampSelection::BACK_ONLY, LampSelection::BOTH}) {
    for (bool power : {false, true}) {
      for (auto first : {Section::FRONT, Section::BACK}) {
        const auto second = first == Section::FRONT ? Section::BACK : Section::FRONT;
        auto state = initial_state();
        state.selection = selection;
        state.power = power;

        state.set_light(first, true, state.brightness(first));
        state.set_light(second, true, state.brightness(second));
        CHECK(state.is_on(Section::FRONT));
        CHECK(state.is_on(Section::BACK));
        CHECK(state.selection == LampSelection::BOTH);

        state.set_light(first, false, state.brightness(first));
        CHECK(!state.is_on(first));
        CHECK(state.is_on(second));
        const auto last_selection = state.selection;
        state.set_light(second, false, state.brightness(second));
        CHECK(!state.power);
        CHECK(state.selection == last_selection);
        CHECK(state.front_brightness == 40);
        CHECK(state.back_brightness == 70);
      }
    }
  }
}

static void brightness_changes_preserve_the_other_section() {
  auto state = initial_state();
  state.selection = LampSelection::FRONT_ONLY;
  state.set_light(Section::FRONT, true, 255);
  CHECK(state.front_brightness == 100);
  CHECK(state.back_brightness == 70);
  CHECK(!state.is_on(Section::BACK));
  state.set_light(Section::FRONT, true, 1);
  CHECK(state.front_brightness == 1);
  CHECK(state.back_brightness == 70);
}

static void recovery_retains_requests_but_requires_a_new_baseline() {
  LampStateModel model;
  const auto confirmed = initial_state();
  model.observe(confirmed);
  auto local = confirmed;
  local.power = false;
  model.request(local);

  model.invalidate();
  CHECK(!model.initialized());
  CHECK(!model.observed());
  CHECK(model.requested() == local);
  model.request(local);
  CHECK(!model.initialized());
  CHECK(model.observe(confirmed));
  CHECK(model.initialized());
  CHECK(model.requested() == confirmed);
  CHECK(model.observed() == confirmed);

  model.restore(local);
  CHECK(!model.initialized());
  CHECK(!model.observed());
  CHECK(model.requested() == local);
}

int main() {
  restored_settings_require_received_state();
  requests_do_not_mutate_confirmed_settings();
  first_confirmation_records_unchanged_settings();
  lamp_readback_reconciles_a_failed_request();
  zero_brightness_preserves_levels_and_selection();
  inactive_brightness_intent_survives_readback_until_the_next_on();
  grouped_commands_work_in_either_order();
  brightness_changes_preserve_the_other_section();
  recovery_retains_requests_but_requires_a_new_baseline();
  std::cout << "Lamp state tests passed\n";
}
