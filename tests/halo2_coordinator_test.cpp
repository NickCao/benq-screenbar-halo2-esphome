#include <iostream>
#include "bridge_fixture.h"

using namespace esphome::halo2;
using namespace halo2_test;

static void boot_waits_for_fresh_state_and_does_not_echo_publications() {
  BridgeFixture f;
  f.front.make_call().set_state(true).set_brightness(.85f).perform();
  f.bridge.control_ultrasonic(4);
  f.bridge.resend();
  CHECK(!f.bridge.accepts_commands());
  CHECK(!f.ultrasonic.state);

  // Initialization must only query the lamp, despite those early UI requests.
  f.next_tx(Command::STATUS);
  f.reply(baseline(false));
  f.next_tx(Command::STATUS);
  CHECK(!f.bridge.accepts_commands());  // The refresh ACK is never a baseline.
  f.reply(baseline());
  f.wait_until([&]() { return f.bridge.accepts_commands(); });
  f.expect_lights(baseline());
  CHECK(f.ultrasonic.state == 0);
  f.advance(300);
  CHECK(f.chip.transmissions.size() == 2);

  // Unsolicited replies cannot change the lights or create commands.
  f.receive(baseline(false), true);
  f.expect_lights(baseline());
  f.advance(100);
  CHECK(f.chip.transmissions.size() == 2);
}

static void a_command_during_status_tx_takes_priority_over_the_stale_reply(bool during_read) {
  BridgeFixture f;
  f.establish_baseline(baseline());
  f.bridge.update();
  f.next_tx(Command::STATUS);
  if (during_read) {
    f.reply(baseline());
    f.next_tx(Command::STATUS);
  }
  const size_t count = f.chip.transmissions.size();
  auto desired = baseline();
  desired.front_brightness = 55;
  f.front.make_call().set_brightness(.55f).perform();
  f.advance(10);  // Still inside the radio's 50 ms TX deadline.
  CHECK(f.chip.transmissions.size() == count);

  const uint32_t completed_at = esphome::millis();
  f.reply(baseline(false));
  const auto command = f.next_tx(Command::SETTINGS);
  CHECK(command.packet().state == desired);
  CHECK(command.at - completed_at < 200);  // A poll completion must not start the command cooldown.
  CHECK(f.status_count("Command sent") == 0);
  f.expect_lights(desired);

  f.finish_command();
  f.readback(desired);
  f.expect_lights(desired);
  f.advance(200);
  CHECK(f.chip.transmissions.size() == count + 3);  // One command and one refresh/read cycle.
}

static void new_intent_during_a_batch_waits_for_both_packets_and_the_cooldown() {
  BridgeFixture f;
  f.establish_baseline(baseline(false));
  f.front.make_call().set_state(true).perform();
  const auto settings = f.next_tx(Command::SETTINGS);
  auto first = baseline();
  first.selection = LampSelection::FRONT_ONLY;
  CHECK(settings.packet().state == first);

  // A second light command must not alter either frame of the already submitted batch.
  f.back.make_call().set_state(true).set_brightness(.80f).perform();
  f.chip.finish_tx();
  const auto power = f.next_tx(Command::POWER);
  CHECK(power.packet().state == first);
  CHECK(f.status_count("Command sent") == 0);
  const size_t count = f.chip.transmissions.size();
  f.finish_command();
  CHECK(f.status_count("Command sent") == 1);
  const uint32_t finished_at = esphome::millis();

  f.bridge.update();
  f.advance(900);
  CHECK(f.chip.transmissions.size() == count);  // Polling also yields to pending intent.
  const auto next = f.next_tx(Command::SETTINGS);
  auto latest = baseline();
  latest.back_brightness = 80;
  CHECK(next.packet().state == latest);
  CHECK(next.at - finished_at >= 1000);
  f.finish_command();
  f.readback(latest);
  f.expect_lights(latest);
}

static void failure_between_power_on_packets_discards_the_batch_and_pending_intent() {
  BridgeFixture f;
  f.establish_baseline(baseline(false));
  f.front.make_call().set_state(true).perform();
  f.next_tx(Command::SETTINGS);
  f.bridge.control_ultrasonic(4);  // Queue additional settings and a timeout behind the batch.
  f.chip.finish_tx();
  f.chip.fail();  // The settings packet finished; power has not been transmitted.
  f.wait_until([&]() { return !f.bridge.accepts_commands(); });
  CHECK(f.bridge.status_has_warning());
  CHECK(f.status_count("Command sent") == 0);
  const size_t count = f.chip.transmissions.size();
  const uint32_t failed_at = esphome::millis();

  f.front.make_call().set_brightness(.90f).perform();
  f.bridge.resend();
  f.bridge.control_ultrasonic(2);
  f.advance(900);
  CHECK(f.chip.transmissions.size() == count);
  CHECK(f.chip.reset_count == 1);
  f.wait_until([&]() { return f.chip.reset_count == 2; });
  CHECK(esphome::millis() - failed_at >= 1000);
  CHECK(!f.bridge.accepts_commands());

  auto actual = baseline(false);
  actual.selection = LampSelection::FRONT_ONLY;
  f.establish_baseline(actual);
  CHECK(!f.bridge.status_has_warning());
  CHECK(f.chip.address == BridgeFixture::ADDRESS);
  CHECK(f.ultrasonic.state == 0);
  f.advance(1100);
  CHECK(f.chip.transmissions.size() == count + 2);  // Recovery only queried; it replayed no commands.
}

static void discovery_during_tx_discards_old_work_and_ignores_its_completion(Command command) {
  BridgeFixture f;
  f.establish_baseline(baseline(command != Command::POWER));
  if (command != Command::STATUS) {
    if (command == Command::POWER)
      f.front.make_call().set_state(true).perform();
    else
      f.front.make_call().set_brightness(.55f).perform();
    f.next_tx(Command::SETTINGS);
    f.bridge.control_ultrasonic(4);
  } else {
    f.bridge.update();
    f.next_tx(Command::STATUS);
    f.reply(baseline());
    f.next_tx(Command::STATUS);
  }
  const size_t count = f.chip.transmissions.size() + (command == Command::POWER ? 1 : 0);
  f.bridge.start_discovery();
  CHECK(!f.bridge.accepts_commands());
  f.bridge.resend();
  f.reply(baseline(false));
  if (command == Command::POWER) {
    // The driver finishes the submitted batch before entering discovery.
    // Neither packet's completion may report success for canceled work.
    f.next_tx(Command::POWER);
    CHECK(!f.bridge.accepts_commands());
    CHECK(f.status_count("Command sent") == 0);
    f.reply(baseline(false));
  }
  f.wait_until([&]() { return f.chip.receiving && f.chip.rx_length == LR1121Radio::DISCOVERY_RX_BYTES; });
  CHECK(f.status_count("Command sent") == 0);
  CHECK(f.chip.transmissions.size() == count);

  const protocol::Address learned{0xAB, 0xCD, 0x12, 0x34};
  auto state = baseline();
  state.front_brightness = 25;
  state.ultrasonic_enabled = true;
  state.ultrasonic_timeout = UltrasonicTimeout::MINUTES_3;
  f.discover(learned, state);
  f.discover(learned, state);
  CHECK(!f.bridge.accepts_commands());
  f.discover(learned, state);
  CHECK(f.bridge.accepts_commands());
  f.expect_lights(state);
  CHECK(f.ultrasonic.state == 1);
  CHECK(f.address.state == "AB:CD:12:34 / 2405 MHz");
  f.advance(500);
  CHECK(f.chip.transmissions.size() == count);
  f.readback(state);
  CHECK(f.chip.transmissions.size() == count + 2);
  CHECK(f.chip.transmissions.back().address == learned);
  f.expect_lights(state);
}

static void discovery_recovery_restarts_the_scan_and_cancels_the_old_dwell() {
  BridgeFixture f(true);
  f.wait_until([&]() { return f.chip.receiving && f.chip.rx_length == LR1121Radio::DISCOVERY_RX_BYTES; });
  f.advance(2800);
  f.chip.fail();
  f.wait_until([&]() { return f.bridge.status_has_warning(); });
  f.wait_until([&]() { return f.chip.reset_count == 2; });
  f.wait_until([&]() { return f.chip.receiving && f.chip.rx_length == LR1121Radio::DISCOVERY_RX_BYTES; });
  CHECK(!f.bridge.accepts_commands());
  CHECK(f.chip.channel == 5 && f.chip.sync == 0xAA);
  f.advance(2800);
  CHECK(f.chip.channel == 5 && f.chip.sync == 0xAA);
  f.wait_until([&]() { return f.chip.receiving && f.chip.sync == 0x55; });
  CHECK(f.chip.channel == 5);
  CHECK(f.chip.transmissions.empty());
}

static void missed_polls_retain_state_and_a_fresh_reply_clears_the_warning() {
  BridgeFixture f;
  f.establish_baseline(baseline());
  for (unsigned cycle = 1; cycle <= 3; ++cycle) {
    f.bridge.update();
    f.next_tx(Command::STATUS);
    f.chip.finish_tx();  // No reply; TX_DONE and the RX-window timeout are separate.
    f.advance(300);
    f.expect_lights(baseline());
    CHECK(f.bridge.status_has_warning() == (cycle == 3));
  }
  CHECK(f.status.state == "Lamp status unavailable; retaining last known state");
  auto actual = baseline(false);
  f.bridge.update();
  f.readback(actual);
  f.expect_lights(actual);
  CHECK(!f.bridge.status_has_warning());

  f.bridge.update();
  f.next_tx(Command::STATUS);
  f.chip.finish_tx();
  f.advance(300);
  CHECK(!f.bridge.status_has_warning());  // Successful readback reset the consecutive-failure count.
}

static void controller_updates_publish_without_echo_and_request_a_fresh_poll() {
  BridgeFixture f;
  f.establish_baseline(baseline());
  const size_t count = f.chip.transmissions.size();
  auto controller = baseline();
  controller.back_brightness = 75;
  controller.color_temperature = 4700;
  controller.ultrasonic_enabled = true;
  f.receive(controller);
  f.expect_lights(controller);
  CHECK(f.ultrasonic.state == 2);
  f.advance(100);
  CHECK(f.chip.transmissions.size() == count);
  f.readback(controller);
  f.advance(200);
  CHECK(f.chip.transmissions.size() == count + 2);  // Reflected LightCalls never queued a command.
  f.expect_lights(controller);
}

static void wrong_pid_and_stale_command_replies_cannot_publish_state() {
  BridgeFixture f;
  f.establish_baseline(baseline());
  const auto changed = baseline(false);
  f.bridge.update();
  f.next_tx(Command::STATUS);
  const size_t count = f.chip.transmissions.size();
  f.reply(changed, Command::STATUS, protocol::PCF_PID_MASK);
  f.advance(300);
  f.expect_lights(baseline());
  CHECK(f.chip.transmissions.size() == count);  // A mismatched refresh ACK cannot authorize a read.

  f.bridge.update();
  f.next_tx(Command::STATUS);
  f.reply(changed, Command::POWER);
  f.next_tx(Command::STATUS);
  f.expect_lights(baseline());
  f.reply(changed, Command::SETTINGS);
  const uint32_t stale_at = esphome::millis();
  const auto retry = f.next_tx(Command::STATUS);
  CHECK(retry.at - stale_at >= StatusPoll::SETTLE_MS);
  f.expect_lights(baseline());
  const uint32_t rx_count = f.bridge.get_radio().rx_count();
  f.reply(changed);
  f.wait_until([&]() { return f.bridge.get_radio().rx_count() > rx_count; });
  f.expect_lights(changed);
}

static void temperature_rounding_leaves_a_peer_transition_running() {
  BridgeFixture f;
  f.establish_baseline(baseline());
  f.back.make_call().set_brightness(.20f).set_transition_length(4000).perform();
  CHECK(!f.back.is_transformer_active());
  const size_t before_sample = f.chip.transmissions.size();
  f.bridge.update();
  f.advance(100);
  CHECK(f.chip.transmissions.size() == before_sample);  // Also guard the interval before the first fade sample.
  auto sample = f.back.current_values;
  sample.brightness = .40f;
  f.back.transition_sample(sample);
  f.front.make_call().set_color_temperature(1000000.0f / 4503).perform();
  CHECK(f.back.is_transformer_active());
  CHECK(f.back.current_values.brightness == .40f);
  CHECK(f.back.remote_values.brightness == .20f);
  CHECK(std::abs(f.front.remote_values.temperature - 1000000.0f / 4500) < .001f);
  f.next_tx(Command::SETTINGS);
  f.finish_command();
  const size_t count = f.chip.transmissions.size();
  f.bridge.update();
  f.advance(1100);
  CHECK(f.chip.transmissions.size() == count);  // No polling during the fade.

  f.back.transition_sample(f.back.remote_values, true);
  const auto final = f.next_tx(Command::SETTINGS);
  CHECK(final.packet().state.back_brightness == 20);
  f.finish_command();
  auto expected = baseline();
  expected.back_brightness = 20;
  f.readback(expected);
  f.expect_lights(expected);
}

static void shared_temperature_merges_the_peer_target_when_it_ends_a_fade(bool turn_off) {
  BridgeFixture f;
  f.establish_baseline(baseline());
  f.back.make_call().set_state(!turn_off).set_brightness(turn_off ? .62f : .20f).set_transition_length(2000).perform();
  auto sample = f.back.current_values;
  sample.brightness = .40f;
  f.back.transition_sample(sample);
  f.front.make_call().set_color_temperature(1000000.0f / 4725).perform();
  CHECK(!f.back.is_transformer_active());
  auto expected = baseline();
  expected.color_temperature = 4725;
  if (turn_off)
    expected.selection = LampSelection::FRONT_ONLY;
  else
    expected.back_brightness = 20;
  f.expect_lights(expected);
  const auto tx = f.next_tx(Command::SETTINGS);
  CHECK(tx.packet().state == expected);
  f.finish_command();
  f.readback(expected);
  f.expect_lights(expected);
}

static void deferred_write_captures_the_final_fade_target() {
  BridgeFixture f;
  f.establish_baseline(baseline());
  f.back.make_call().set_state(false).set_color_temperature(1000000.0f / 4725).set_transition_length(2000).perform();
  // The OFF transition's last sample still has its original temperature.
  // ESPHome replaces that sample with the requested target before write_state().
  auto sample = f.back.current_values;
  sample.brightness = 0;
  f.back.transition_sample(sample, true);
  const auto tx = f.next_tx(Command::SETTINGS);
  auto expected = baseline();
  expected.selection = LampSelection::FRONT_ONLY;
  expected.color_temperature = 4725;
  CHECK(tx.packet().state == expected);
  f.expect_lights(expected);
  f.finish_command();
  f.readback(expected);
  f.expect_lights(expected);
}

int main() {
  const auto run = [](const char *name, auto test) {
    scenario = name;
    test();
    std::cout << "  Passed: " << name << '\n';
  };
  run("boot baseline and publication guards", boot_waits_for_fresh_state_and_does_not_echo_publications);
  run("command during refresh TX", []() { a_command_during_status_tx_takes_priority_over_the_stale_reply(false); });
  run("command during read TX", []() { a_command_during_status_tx_takes_priority_over_the_stale_reply(true); });
  run("batch completion and queued intent", new_intent_during_a_batch_waits_for_both_packets_and_the_cooldown);
  run("failure between power-on packets", failure_between_power_on_packets_discards_the_batch_and_pending_intent);
  run("discovery during command TX",
      []() { discovery_during_tx_discards_old_work_and_ignores_its_completion(Command::SETTINGS); });
  run("discovery during read TX",
      []() { discovery_during_tx_discards_old_work_and_ignores_its_completion(Command::STATUS); });
  run("discovery during power-on batch",
      []() { discovery_during_tx_discards_old_work_and_ignores_its_completion(Command::POWER); });
  run("discovery recovery and dwell timer", discovery_recovery_restarts_the_scan_and_cancels_the_old_dwell);
  run("poll failures and recovery", missed_polls_retain_state_and_a_fresh_reply_clears_the_warning);
  run("controller update without echo", controller_updates_publish_without_echo_and_request_a_fresh_poll);
  run("reply PID and freshness filtering", wrong_pid_and_stale_command_replies_cannot_publish_state);
  run("rounding during peer transition", temperature_rounding_leaves_a_peer_transition_running);
  run("temperature change during peer OFF fade",
      []() { shared_temperature_merges_the_peer_target_when_it_ends_a_fade(true); });
  run("temperature change during peer dimming",
      []() { shared_temperature_merges_the_peer_target_when_it_ends_a_fade(false); });
  run("final fade values in deferred write", deferred_write_captures_the_final_fade_target);
  std::cout << "Halo2 coordinator tests passed\n";
}
