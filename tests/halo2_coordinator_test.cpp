#include <iostream>
#include <limits>
#include "bridge_fixture.h"
#include "favorite_captures.h"

using namespace esphome::halo2;
using namespace halo2_test;

static void boot_waits_for_fresh_state_and_does_not_echo_publications() {
  BridgeFixture f;
  f.master.make_call().set_state(true).set_brightness(.85f).perform();
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
  f.expect_state(baseline());
  CHECK(f.ultrasonic.state == 0);
  f.advance(300);
  CHECK(f.chip.transmissions.size() == 2);

  // Unsolicited replies cannot change the lights or create commands.
  f.receive(baseline(false), true);
  f.expect_state(baseline());
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
  f.bridge.control_brightness(Section::FRONT, 55);
  f.advance(10);  // Still inside the radio's 50 ms TX deadline.
  CHECK(f.chip.transmissions.size() == count);

  const uint32_t completed_at = esphome::millis();
  f.reply(baseline(false));
  const auto command = f.next_tx(Command::SETTINGS);
  CHECK(command.packet().state == desired);
  CHECK(command.at - completed_at < 200);  // A poll completion must not start the command cooldown.
  CHECK(f.status_count("Command sent") == 0);
  f.expect_state(desired);

  f.finish_command();
  f.readback(desired);
  f.expect_state(desired);
  f.advance(200);
  CHECK(f.chip.transmissions.size() == count + 3);  // One command and one refresh/read cycle.
}

static void new_intent_during_a_batch_waits_for_both_packets_and_the_cooldown() {
  BridgeFixture f;
  f.establish_baseline(baseline(false));
  f.master.make_call().set_state(true).perform();
  const auto settings = f.next_tx(Command::SETTINGS);
  auto first = baseline();
  CHECK(settings.packet().state == first);

  // A brightness setting must not alter either frame of the already submitted batch.
  f.bridge.control_brightness(Section::BACK, 80);
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
  f.expect_state(latest);
}

static void failure_between_power_on_packets_discards_the_batch_and_pending_intent() {
  BridgeFixture f;
  f.establish_baseline(baseline(false));
  f.master.make_call().set_state(true).perform();
  f.next_tx(Command::SETTINGS);
  f.bridge.control_ultrasonic(4);  // Queue additional settings and a timeout behind the batch.
  f.chip.finish_tx();
  f.chip.fail();  // The settings packet finished; power has not been transmitted.
  f.wait_until([&]() { return !f.bridge.accepts_commands(); });
  CHECK(f.bridge.status_has_warning());
  CHECK(f.status_count("Command sent") == 0);
  const size_t count = f.chip.transmissions.size();
  const uint32_t failed_at = esphome::millis();

  f.master.make_call().set_brightness(.90f).perform();
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
      f.master.make_call().set_state(true).perform();
    else
      f.bridge.control_brightness(Section::FRONT, 55);
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
  f.expect_state(state);
  CHECK(f.ultrasonic.state == 1);
  CHECK(f.address.state == "AB:CD:12:34 / 2405 MHz");
  f.advance(500);
  CHECK(f.chip.transmissions.size() == count);
  f.readback(state);
  CHECK(f.chip.transmissions.size() == count + 2);
  CHECK(f.chip.transmissions.back().address == learned);
  f.expect_state(state);
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
    f.expect_state(baseline());
    CHECK(f.bridge.status_has_warning() == (cycle == 3));
  }
  CHECK(f.status.state == "Lamp status unavailable; retaining last known state");
  auto actual = baseline(false);
  f.bridge.update();
  f.readback(actual);
  f.expect_state(actual);
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
  f.expect_state(controller);
  CHECK(f.ultrasonic.state == 2);
  f.advance(100);
  CHECK(f.chip.transmissions.size() == count);
  f.readback(controller);
  f.advance(200);
  CHECK(f.chip.transmissions.size() == count + 2);  // Reflected LightCalls never queued a command.
  f.expect_state(controller);
}

static void captured_favorite_updates_publish_without_echo_and_reconcile_from_the_lamp() {
  BridgeFixture f(false, FAVORITE_ADDRESS);
  f.establish_baseline(baseline());
  for (const auto &capture : FAVORITE_CAPTURES) {
    const size_t count = f.chip.transmissions.size();
    const auto controller = favorite_state(capture);
    f.receive(capture.frame);
    f.expect_state(controller);
    CHECK(f.status.state == "Controller update received");
    CHECK(f.ultrasonic.state == 2);
    f.advance(100);
    CHECK(f.chip.transmissions.size() == count);

    // A controller snapshot does not prove the lamp applied every field.
    // Fresh status must reconcile it without echoing settings back.
    auto actual = controller;
    actual.color_temperature = 4500;
    actual.ultrasonic_enabled = false;
    f.readback(actual);
    f.expect_state(actual);
    CHECK(f.ultrasonic.state == 0);
    f.advance(200);
    CHECK(f.chip.transmissions.size() == count + 2);
    CHECK(f.status_count("Command sent") == 0);
  }
}

static void favorite_replies_can_refresh_but_cannot_publish_until_a_status_reply(uint8_t command) {
  BridgeFixture f;
  f.establish_baseline(baseline());
  const auto stale = baseline(false);
  f.bridge.update();
  f.next_tx(Command::STATUS);
  f.reply(stale, command, 0, true);
  f.next_tx(Command::STATUS);
  f.expect_state(baseline());  // A favorite ACK can refresh, never publish.

  f.reply(stale, command, 0, true);
  const uint32_t received_at = esphome::millis();
  const auto retry = f.next_tx(Command::STATUS);
  CHECK(retry.at - received_at >= StatusPoll::SETTLE_MS);
  f.expect_state(baseline());

  const uint32_t rx_count = f.bridge.get_radio().rx_count();
  f.reply(stale);
  f.wait_until([&]() { return f.bridge.get_radio().rx_count() > rx_count; });
  f.expect_state(stale);
  CHECK(f.status.state == "Lamp status received");
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
  f.expect_state(baseline());
  CHECK(f.chip.transmissions.size() == count);  // A mismatched refresh ACK cannot authorize a read.

  f.bridge.update();
  f.next_tx(Command::STATUS);
  f.reply(changed, Command::POWER);
  f.next_tx(Command::STATUS);
  f.expect_state(baseline());
  f.reply(changed, Command::SETTINGS);
  const uint32_t stale_at = esphome::millis();
  const auto retry = f.next_tx(Command::STATUS);
  CHECK(retry.at - stale_at >= StatusPoll::SETTLE_MS);
  f.expect_state(baseline());
  const uint32_t rx_count = f.bridge.get_radio().rx_count();
  f.reply(changed);
  f.wait_until([&]() { return f.bridge.get_radio().rx_count() > rx_count; });
  f.expect_state(changed);
}

static void master_off_retains_presence_wake_settings(LampSelection selection, bool fade) {
  BridgeFixture f;
  auto original = baseline();
  original.selection = selection;
  original.ultrasonic_enabled = true;
  f.establish_baseline(original);
  const size_t count = f.chip.transmissions.size();
  f.master.make_call().set_state(false).set_transition_length(fade ? 2000 : 0).perform();
  if (fade) {
    auto sample = f.master.current_values;
    sample.brightness = .15f;
    f.master.transition_sample(sample);
    f.advance(150);
    CHECK(f.chip.transmissions.size() == count);  // OFF samples never overwrite the wake profile.
    sample.brightness = .0001f;
    f.master.transition_sample(sample, true);
  }
  auto off = original;
  off.power = false;
  CHECK(f.next_tx(Command::POWER).packet().state == off);
  f.expect_state(off);
  f.finish_command();
  f.readback(off);
  f.expect_state(off);
  CHECK(f.chip.transmissions.size() == count + 3);  // POWER only, then refresh/read; no SETTINGS.

  // Presence wakes the lamp itself. Polling observes it without transmitting ON.
  f.bridge.update();
  f.readback(original);
  f.expect_state(original);
  f.advance(200);
  CHECK(f.chip.transmissions.size() == count + 5);
}

static void zero_master_brightness_retains_the_profile_for_plain_on() {
  BridgeFixture f;
  f.establish_baseline(baseline());
  f.master.make_call().set_brightness(0).perform();
  auto off = baseline(false);
  CHECK(f.next_tx(Command::POWER).packet().state == off);
  f.expect_state(off);
  f.finish_command();
  f.master.make_call().set_state(true).perform();
  CHECK(f.next_tx(Command::SETTINGS).packet().state == baseline());
  f.chip.finish_tx();
  CHECK(f.next_tx(Command::POWER).packet().state == baseline());
  f.finish_command();
  f.readback(baseline());
  f.expect_state(baseline());
}

static void native_dimming_and_controller_changes_use_the_current_profile() {
  BridgeFixture f;
  auto original = baseline();
  original.front_brightness = 30;
  original.back_brightness = 80;
  f.establish_baseline(original);
  f.master.make_call().set_state(true).set_brightness(.40f).perform();
  auto expected = original;
  expected.front_brightness = 15;
  expected.back_brightness = 40;
  CHECK(f.next_tx(Command::SETTINGS).packet().state == expected);
  f.finish_command();
  f.readback(expected);
  f.expect_state(expected);

  auto controller = original;
  controller.front_brightness = 60;
  controller.back_brightness = 20;
  const size_t count = f.chip.transmissions.size();
  f.receive(controller);
  f.expect_state(controller);
  f.advance(100);
  CHECK(f.chip.transmissions.size() == count);
  f.master.make_call().set_state(true).set_brightness(.30f).perform();
  expected = controller;
  expected.front_brightness = 30;
  expected.back_brightness = 10;
  CHECK(f.next_tx(Command::SETTINGS).packet().state == expected);
  f.finish_command();
  f.readback(expected);
  f.expect_state(expected);
}

static void native_dimming_only_changes_selected_sections(LampSelection selection) {
  BridgeFixture f;
  auto original = baseline(false);
  original.selection = selection;
  f.establish_baseline(original);
  f.master.make_call().set_state(true).set_brightness(.20f).perform();
  auto expected = original;
  expected.power = true;
  if (selection == LampSelection::FRONT_ONLY)
    expected.front_brightness = 20;
  else
    expected.back_brightness = 20;
  CHECK(f.next_tx(Command::SETTINGS).packet().state == expected);
  f.chip.finish_tx();
  CHECK(f.next_tx(Command::POWER).packet().state == expected);
  f.finish_command();
  f.readback(expected);
  f.expect_state(expected);
}

static void section_selection_and_inactive_readback_reflect_the_lamp() {
  BridgeFixture f;
  f.establish_baseline(baseline());
  f.bridge.control_selection(static_cast<size_t>(LampSelection::BACK_ONLY));
  auto expected = baseline();
  expected.selection = LampSelection::BACK_ONLY;
  CHECK(f.next_tx(Command::SETTINGS).packet().state == expected);
  f.finish_command();
  expected.front_brightness = 1;  // Always display the actual inactive section's stored level.
  f.readback(expected);
  f.expect_state(expected);
  const size_t count = f.chip.transmissions.size();
  f.bridge.control_brightness(Section::FRONT, 80);  // The lamp cannot write an inactive section's brightness.
  f.advance(150);
  CHECK(f.chip.transmissions.size() == count);
  f.expect_state(expected);

  f.bridge.control_selection(static_cast<size_t>(LampSelection::BOTH));
  expected.selection = LampSelection::BOTH;
  CHECK(f.next_tx(Command::SETTINGS).packet().state == expected);
  f.finish_command();
  f.readback(expected);
  f.bridge.control_brightness(Section::FRONT, 80);
  expected.front_brightness = 80;
  CHECK(f.next_tx(Command::SETTINGS).packet().state == expected);
  f.finish_command();
  f.readback(expected);
  f.expect_state(expected);
}

static void section_settings_while_off_do_not_turn_on_the_lamp() {
  BridgeFixture f;
  f.establish_baseline(baseline(false));
  const size_t count = f.chip.transmissions.size();
  f.bridge.control_selection(static_cast<size_t>(LampSelection::BACK_ONLY));
  f.bridge.control_brightness(Section::BACK, 80);
  f.bridge.control_selection(static_cast<size_t>(LampSelection::BOTH));
  f.bridge.control_brightness(Section::FRONT, 25);
  auto expected = baseline(false);
  expected.front_brightness = 25;
  expected.back_brightness = 80;
  CHECK(f.next_tx(Command::SETTINGS).packet().state == expected);
  f.finish_command();
  f.readback(expected);
  f.expect_state(expected);
  CHECK(f.chip.transmissions.size() == count + 3);
}

static void selection_changes_preserve_power_and_levels(bool power) {
  BridgeFixture f;
  auto expected = baseline(power);
  f.establish_baseline(expected);
  for (auto selection : {LampSelection::FRONT_ONLY, LampSelection::BACK_ONLY, LampSelection::BOTH}) {
    f.bridge.control_selection(static_cast<size_t>(selection));
    expected.selection = selection;
    CHECK(f.next_tx(Command::SETTINGS).packet().state == expected);
    f.finish_command();
    f.readback(expected);
    f.expect_state(expected);
  }
}

static void controller_modes_and_arbitrary_levels_map_directly_to_settings() {
  BridgeFixture f;
  f.establish_baseline(baseline());
  for (auto selection : {LampSelection::FRONT_ONLY, LampSelection::BACK_ONLY, LampSelection::BOTH}) {
    auto controller = baseline(false);
    controller.selection = selection;
    controller.front_brightness = 1;
    controller.back_brightness = 99;
    const size_t count = f.chip.transmissions.size();
    f.receive(controller);
    f.expect_state(controller);
    f.advance(100);
    CHECK(f.chip.transmissions.size() == count);
    f.readback(controller);
    f.expect_state(controller);
  }
}

static void fade_samples_keep_the_original_ratio_and_deferred_final_target() {
  BridgeFixture f;
  f.establish_baseline(baseline());
  f.master.make_call().set_brightness(.31f).set_transition_length(4000).perform();
  CHECK(!f.master.is_transformer_active());
  const size_t before_sample = f.chip.transmissions.size();
  f.bridge.update();
  f.advance(100);
  CHECK(f.chip.transmissions.size() == before_sample);  // Before the first sample also blocks polling.
  auto sample = f.master.current_values;
  sample.brightness = .01f;
  f.master.transition_sample(sample);
  const auto low = f.next_tx(Command::SETTINGS).packet().state;
  CHECK(low.front_brightness == 1 && low.back_brightness == 1);
  f.finish_command();
  sample.brightness = .20f;
  f.master.transition_sample(sample);
  f.advance(100);
  sample.brightness = .30f;  // update_state sample differs from the final .31 target.
  f.master.transition_sample(sample, true);
  auto expected = baseline();
  expected.front_brightness = 19;
  expected.back_brightness = 31;
  CHECK(f.next_tx(Command::SETTINGS).packet().state == expected);
  f.finish_command();
  f.readback(expected);
  f.expect_state(expected);
}

static void a_section_setting_finishes_a_pending_master_fade_at_its_target() {
  BridgeFixture f;
  f.establish_baseline(baseline());
  f.master.make_call().set_brightness(.20f).set_color_temperature(1000000.0f / 4727)
      .set_transition_length(4000).perform();
  CHECK(!f.master.is_transformer_active());
  f.bridge.control_brightness(Section::FRONT, 55);
  auto expected = baseline();
  expected.front_brightness = 55;
  expected.back_brightness = 20;
  expected.color_temperature = 4725;
  f.expect_state(expected);
  CHECK(!f.master.is_transformer_active());
  CHECK(f.master.current_values == f.master.remote_values);
  CHECK(f.next_tx(Command::SETTINGS).packet().state == expected);
  f.finish_command();
  f.readback(expected);
  f.expect_state(expected);
}

static void temperature_call_during_a_fade_preserves_the_original_ratio() {
  BridgeFixture f;
  f.establish_baseline(baseline());
  f.master.make_call().set_brightness(.31f).set_transition_length(4000).perform();
  auto sample = f.master.current_values;
  sample.brightness = .01f;
  f.master.transition_sample(sample);  // Both rounded outputs are 1%; that is not the new ratio.
  f.master.make_call().set_color_temperature(1000000.0f / 4725).perform();
  auto expected = baseline();
  expected.front_brightness = 19;
  expected.back_brightness = 31;
  expected.color_temperature = 4725;
  CHECK(f.master.current_values == f.master.remote_values);
  CHECK(f.next_tx(Command::SETTINGS).packet().state == expected);
  f.finish_command();
  f.readback(expected);
  f.expect_state(expected);
}

static void changing_mode_during_a_fade_reconciles_the_inactive_stored_level() {
  BridgeFixture f;
  f.establish_baseline(baseline());
  f.master.make_call().set_brightness(.20f).set_transition_length(4000).perform();
  f.bridge.control_selection(static_cast<size_t>(LampSelection::FRONT_ONLY));
  auto requested = baseline();
  requested.front_brightness = 12;
  requested.back_brightness = 20;
  requested.selection = LampSelection::FRONT_ONLY;
  CHECK(f.next_tx(Command::SETTINGS).packet().state == requested);
  f.finish_command();
  // The new selection leaves the rear at its previous lamp-stored level.
  auto actual = requested;
  actual.back_brightness = 62;
  f.readback(actual);
  f.expect_state(actual);
}

static void native_minimum_brightness_keeps_both_sections_selected() {
  BridgeFixture f;
  f.establish_baseline(baseline());
  f.master.make_call().set_state(true).set_brightness(1.0f / 255).perform();
  auto expected = baseline();
  expected.front_brightness = expected.back_brightness = 1;
  CHECK(f.next_tx(Command::SETTINGS).packet().state == expected);
  f.finish_command();
  f.readback(expected);
  f.expect_state(expected);
}

static void temperature_rounding_and_invalid_numbers_do_not_change_the_profile() {
  BridgeFixture f;
  f.establish_baseline(baseline());
  const size_t count = f.chip.transmissions.size();
  f.master.make_call().set_color_temperature(1000000.0f / 4503).perform();
  f.bridge.control_brightness(Section::FRONT, -1);
  f.bridge.control_brightness(Section::FRONT, 0);
  f.bridge.control_selection(3);
  f.bridge.control_brightness(Section::BACK, 101);
  f.bridge.control_brightness(Section::FRONT, std::numeric_limits<float>::quiet_NaN());
  f.advance(150);
  CHECK(f.chip.transmissions.size() == count);
  f.expect_state(baseline());
  f.master.make_call().set_color_temperature(1000000.0f / 4727).perform();
  auto expected = baseline();
  expected.color_temperature = 4725;
  CHECK(f.next_tx(Command::SETTINGS).packet().state == expected);
  f.finish_command();
  f.readback(expected);
  f.expect_state(expected);
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
  run("captured favorite controller updates", captured_favorite_updates_publish_without_echo_and_reconcile_from_the_lamp);
  run("favorite recall reply freshness", []() { favorite_replies_can_refresh_but_cannot_publish_until_a_status_reply(0x07); });
  run("favorite save reply freshness", []() { favorite_replies_can_refresh_but_cannot_publish_until_a_status_reply(0x08); });
  run("reply PID and freshness filtering", wrong_pid_and_stale_command_replies_cannot_publish_state);
  for (auto selection : {LampSelection::FRONT_ONLY, LampSelection::BACK_ONLY, LampSelection::BOTH}) {
    run("master OFF retains presence wake settings", [selection]() { master_off_retains_presence_wake_settings(selection, false); });
    run("master OFF fade retains presence wake settings", [selection]() { master_off_retains_presence_wake_settings(selection, true); });
  }
  run("zero master brightness and plain ON", zero_master_brightness_retains_the_profile_for_plain_on);
  run("native dimming after controller changes", native_dimming_and_controller_changes_use_the_current_profile);
  for (auto selection : {LampSelection::FRONT_ONLY, LampSelection::BACK_ONLY})
    run("native dimming and ON for a single section", [selection]() { native_dimming_only_changes_selected_sections(selection); });
  run("selection and inactive brightness readback", section_selection_and_inactive_readback_reflect_the_lamp);
  run("controller modes and stored levels", controller_modes_and_arbitrary_levels_map_directly_to_settings);
  run("section settings while globally OFF", section_settings_while_off_do_not_turn_on_the_lamp);
  for (bool power : {false, true})
    run("selection preserves global power and levels", [power]() { selection_changes_preserve_power_and_levels(power); });
  run("fade ratio and deferred final target", fade_samples_keep_the_original_ratio_and_deferred_final_target);
  run("setting during a pending master fade", a_section_setting_finishes_a_pending_master_fade_at_its_target);
  run("temperature interrupts fade without ratio drift", temperature_call_during_a_fade_preserves_the_original_ratio);
  run("mode change during fade reconciles inactive memory", changing_mode_during_a_fade_reconciles_the_inactive_stored_level);
  run("native minimum brightness retains both sections", native_minimum_brightness_keeps_both_sections_selected);
  run("temperature rounding and invalid numbers", temperature_rounding_and_invalid_numbers_do_not_change_the_profile);
  std::cout << "Halo2 coordinator tests passed\n";
}
