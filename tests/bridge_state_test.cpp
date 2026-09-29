#include <cassert>
#include <iostream>
#include "bridge_state.h"

using namespace esphome::halo2;

static void commands_require_a_received_baseline_after_every_restart() {
  BridgeLifecycle lifecycle;
  assert(!lifecycle.active());
  assert(!lifecycle.begin_discovery());
  assert(!lifecycle.on_received_state());
  assert(lifecycle.on_radio_ready());
  assert(lifecycle.linked() && !lifecycle.active());
  assert(lifecycle.on_received_state());
  assert(lifecycle.active());
  assert(lifecycle.on_radio_failure());
  assert(!lifecycle.on_radio_failure());
  assert(!lifecycle.active() && !lifecycle.linked());
  assert(!lifecycle.on_received_state());
  assert(lifecycle.retry());
  assert(lifecycle.on_radio_ready());
  assert(!lifecycle.active());
  assert(lifecycle.on_received_state());
  assert(lifecycle.active());
}

static void discovery_resumes_after_failure_but_a_learned_link_does_not_rescan() {
  BridgeLifecycle lifecycle(true);
  assert(lifecycle.on_radio_failure());
  assert(lifecycle.retry());
  assert(lifecycle.on_radio_ready());
  assert(lifecycle.discovering() && !lifecycle.active() && !lifecycle.linked());
  assert(lifecycle.on_radio_failure());
  assert(lifecycle.retry());
  assert(lifecycle.on_radio_ready());
  assert(lifecycle.discovering());
  assert(lifecycle.on_received_state());
  assert(lifecycle.active());
  assert(lifecycle.on_radio_failure());
  assert(lifecycle.retry());
  assert(lifecycle.on_radio_ready());
  assert(lifecycle.linked() && !lifecycle.discovering() && !lifecycle.active());
  assert(lifecycle.begin_discovery());
  assert(!lifecycle.active() && !lifecycle.linked());
}

static void nested_publication_cannot_reenable_callbacks_or_undo_recovery() {
  BridgeLifecycle lifecycle;
  lifecycle.on_radio_ready();
  lifecycle.on_received_state();
  bool publishing = false;
  {
    ScopedPublication outer(publishing);
    assert(publishing && lifecycle.active());
    {
      ScopedPublication inner(publishing);
      assert(publishing);
    }
    assert(publishing);
    lifecycle.on_radio_failure();
  }
  assert(!publishing && !lifecycle.active());
}

static void replies_must_follow_refresh_and_match_the_current_request() {
  StatusPoll poll;
  poll.schedule(0, 500);
  assert(!poll.due(499) && poll.due(500));
  assert(poll.begin_request(0));
  assert(poll.transmitting());
  assert(!poll.waiting_reply() && !poll.expire(10000));
  assert(poll.on_tx_done(10000));
  assert(!poll.transmitting());
  assert(poll.on_reply(2, true, 10010) == StatusPoll::Reply::IGNORED);
  assert(poll.on_reply(0, true, 10010) == StatusPoll::Reply::NEEDS_READ);
  assert(!poll.waiting_reply() && !poll.due(10509));
  assert(poll.due(10510));
  assert(poll.begin_request(2));
  assert(poll.on_tx_done(10520));
  assert(poll.on_reply(0, true, 10530) == StatusPoll::Reply::IGNORED);
  assert(poll.on_reply(2, true, 10530) == StatusPoll::Reply::STATE);
  assert(poll.idle());
  assert(poll.on_reply(2, true, 10540) == StatusPoll::Reply::IGNORED);
}

static void stale_command_replies_have_a_bounded_read_retry_budget() {
  StatusPoll poll;
  poll.schedule(0, 0);
  poll.begin_request(0);
  poll.on_tx_done(0);
  // The refresh ACK can carry any command. It never establishes lamp state.
  assert(poll.on_reply(0, false, 10) == StatusPoll::Reply::NEEDS_READ);
  uint32_t now = 510;
  for (unsigned attempt = 1; attempt <= StatusPoll::MAX_READ_ATTEMPTS; ++attempt) {
    assert(poll.due(now));
    assert(poll.begin_request(2));
    assert(poll.on_tx_done(now));
    const auto reply = poll.on_reply(2, false, now + 10);
    if (attempt < StatusPoll::MAX_READ_ATTEMPTS) {
      assert(reply == StatusPoll::Reply::NEEDS_READ);
      now += 510;
    } else {
      assert(reply == StatusPoll::Reply::IGNORED);
      assert(poll.waiting_reply());
      assert(!poll.expire(now + 199));
      assert(poll.expire(now + 200));
      assert(poll.idle() && poll.failures() == 1);
    }
  }
}

static void new_intent_cancels_stale_replies_and_failures_clear_on_success() {
  StatusPoll poll;
  for (unsigned cycle = 0; cycle < 5; ++cycle) {
    poll.schedule(0, 0);
    poll.begin_request(0);
    poll.on_tx_done(100);
    assert(!poll.expire(299));
    assert(poll.expire(300));
  }
  assert(poll.failures() == StatusPoll::FAILURE_THRESHOLD);
  poll.schedule(400, 0);
  poll.begin_request(0);
  poll.on_tx_done(400);
  poll.cancel();
  assert(poll.on_reply(0, true, 410) == StatusPoll::Reply::IGNORED);
  assert(!poll.expire(600));
  assert(!poll.on_tx_done(600));

  poll.schedule(600, 0);
  poll.begin_request(2);
  poll.on_tx_done(600);
  poll.on_reply(2, true, 610);
  assert(poll.due(1110));
  poll.begin_request(4);
  poll.on_tx_done(1110);
  assert(poll.on_reply(4, true, 1120) == StatusPoll::Reply::STATE);
  assert(poll.failures() == 0);
}

static void poll_deadlines_work_across_clock_wraparound() {
  const uint32_t start = UINT32_MAX - 100;
  StatusPoll poll;
  poll.schedule(start, 500);
  assert(!poll.due(start + 499));
  assert(poll.due(start + 500));
  poll.schedule(start, 0);
  poll.begin_request(0);
  poll.on_tx_done(start);
  assert(!poll.expire(start + 199));
  assert(poll.expire(start + 200));
}

int main() {
  commands_require_a_received_baseline_after_every_restart();
  discovery_resumes_after_failure_but_a_learned_link_does_not_rescan();
  nested_publication_cannot_reenable_callbacks_or_undo_recovery();
  replies_must_follow_refresh_and_match_the_current_request();
  stale_command_replies_have_a_bounded_read_retry_budget();
  new_intent_cancels_stale_replies_and_failures_clear_on_success();
  poll_deadlines_work_across_clock_wraparound();
  std::cout << "Bridge state tests passed\n";
}
