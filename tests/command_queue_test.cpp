#include <cassert>
#include <iostream>
#include <vector>
#include "command_queue.h"

using namespace esphome::halo2;

static void expect_batch(const std::optional<CommandQueue::Batch> &batch, std::initializer_list<Command> expected) {
  assert(batch);
  assert(std::vector<Command>(batch->begin(), batch->end()) == std::vector<Command>(expected));
}

static void power_on_consumes_settings_but_preserves_the_timeout_command() {
  CommandQueue queue;
  LampState before, after = before;
  after.set_light(Section::FRONT, true, 37);
  after.set_light(Section::BACK, true, 62);
  after.ultrasonic_timeout = UltrasonicTimeout::MINUTES_10;
  queue.request(before, after);
  expect_batch(queue.take(after, 0), {Command::SETTINGS, Command::POWER});
  assert(queue.transmitting() && queue.pending());
  assert(!queue.take(after, 10000));
  assert(queue.on_tx_done(10000));
  assert(!queue.take(after, 10999));
  expect_batch(queue.take(after, 11000), {Command::ULTRASONIC_TIMEOUT});
  assert(!queue.pending() && !queue.idle());
  assert(queue.on_tx_done(11010));
  assert(queue.idle());
}

static void power_off_preserves_every_changed_setting() {
  CommandQueue queue;
  LampState before;
  before.power = true;
  before.ultrasonic_enabled = true;
  auto after = before;
  after.power = false;
  after.front_brightness = 45;
  after.ultrasonic_enabled = false;
  after.ultrasonic_timeout = UltrasonicTimeout::MINUTES_15;
  queue.request(before, after);
  expect_batch(queue.take(after, 0), {Command::POWER});
  queue.on_tx_done(10);
  expect_batch(queue.take(after, 1010), {Command::SETTINGS});
  queue.on_tx_done(1020);
  expect_batch(queue.take(after, 2020), {Command::ULTRASONIC_TIMEOUT});
  queue.on_tx_done(2030);
  assert(queue.idle());
}

static void new_requests_coalesce_without_extending_the_cooldown() {
  CommandQueue queue;
  LampState before, after = before;
  queue.request(before, after);
  assert(queue.idle() && !queue.take(after, 0));
  after.front_brightness = 37;
  queue.request(before, after);
  expect_batch(queue.take(after, 0), {Command::SETTINGS});
  before = after;
  after.front_brightness = 62;
  queue.request(before, after);  // Intent arriving while TX is in flight.
  assert(!queue.take(after, 10000));
  queue.on_tx_done(10000);
  before = after;
  after.back_brightness = 80;
  queue.request(before, after);  // Intent arriving during the cooldown.
  assert(!queue.take(after, 10999));
  expect_batch(queue.take(after, 11000), {Command::SETTINGS});
  assert(!queue.pending());
}

static void only_manual_brightness_or_temperature_cancels_auto_brightness() {
  CommandQueue queue;
  LampState before, after = before;
  queue.auto_brightness();
  after.ultrasonic_enabled = true;
  queue.request(before, after);
  auto batch = queue.take(after, 0);
  expect_batch(batch, {Command::SETTINGS});
  assert(batch->auto_brightness);
  queue.on_tx_done(0);

  for (bool temperature : {false, true}) {
    queue.clear();
    queue.auto_brightness();
    before = after;
    if (temperature)
      after.color_temperature = 4500;
    else
      after.front_brightness = 37;
    queue.request(before, after);
    batch = queue.take(after, 0);
    assert(!batch->auto_brightness);
  }
}

static void resend_reapplies_settings_and_timeout_even_while_off() {
  CommandQueue queue;
  LampState state;
  queue.resend();
  expect_batch(queue.take(state, 0), {Command::POWER});
  queue.on_tx_done(0);
  expect_batch(queue.take(state, 1000), {Command::SETTINGS});
  queue.on_tx_done(1000);
  expect_batch(queue.take(state, 2000), {Command::ULTRASONIC_TIMEOUT});
  queue.on_tx_done(2000);
  assert(queue.idle());
}

static void recovery_drops_in_flight_and_pending_work() {
  CommandQueue queue;
  LampState state;
  queue.resend();
  queue.take(state, 0);
  queue.clear();
  assert(queue.idle());
  assert(!queue.on_tx_done(1000));
  assert(!queue.take(state, 2000));
  queue.auto_brightness();
  const auto batch = queue.take(state, 2000);
  assert(batch && batch->auto_brightness);
}

static void cooldown_handles_clock_wraparound_and_a_zero_interval() {
  CommandQueue queue;
  LampState state;
  const uint32_t start = UINT32_MAX - 100;
  queue.resend();
  queue.take(state, start);
  queue.on_tx_done(start);
  assert(!queue.take(state, start + 999));
  expect_batch(queue.take(state, start + 1000), {Command::SETTINGS});
  queue.on_tx_done(start + 1000);
  queue.set_debounce(0);
  expect_batch(queue.take(state, start + 1000), {Command::ULTRASONIC_TIMEOUT});
}

int main() {
  power_on_consumes_settings_but_preserves_the_timeout_command();
  power_off_preserves_every_changed_setting();
  new_requests_coalesce_without_extending_the_cooldown();
  only_manual_brightness_or_temperature_cancels_auto_brightness();
  resend_reapplies_settings_and_timeout_even_while_off();
  recovery_drops_in_flight_and_pending_work();
  cooldown_handles_clock_wraparound_and_a_zero_interval();
  std::cout << "Command queue tests passed\n";
}
