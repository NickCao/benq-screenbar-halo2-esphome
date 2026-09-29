#pragma once

#include <cstdint>
#include <utility>

namespace esphome::halo2 {

class BridgeLifecycle {
  enum class Phase {
    INITIALIZING,
    INITIALIZING_DISCOVERY,
    AWAITING_STATE,
    ACTIVE,
    DISCOVERING,
    RECOVERING,
    RECOVERING_DISCOVERY,
  };

 public:
  explicit BridgeLifecycle(bool discover = false)
      : phase_(discover ? Phase::INITIALIZING_DISCOVERY : Phase::INITIALIZING) {}
  bool active() const { return phase_ == Phase::ACTIVE; }
  bool linked() const { return phase_ == Phase::AWAITING_STATE || active(); }
  bool discovering() const { return phase_ == Phase::DISCOVERING; }
  bool initializing() const { return phase_ == Phase::INITIALIZING || phase_ == Phase::INITIALIZING_DISCOVERY; }
  bool recovering() const { return phase_ == Phase::RECOVERING || phase_ == Phase::RECOVERING_DISCOVERY; }

  bool on_radio_ready() {
    if (!initializing()) return false;
    phase_ = phase_ == Phase::INITIALIZING_DISCOVERY ? Phase::DISCOVERING : Phase::AWAITING_STATE;
    return true;
  }
  bool begin_discovery() {
    if (!linked() && !discovering()) return false;
    phase_ = Phase::DISCOVERING;
    return true;
  }
  bool on_received_state() {
    if (!linked() && !discovering()) return false;
    phase_ = Phase::ACTIVE;
    return true;
  }
  bool on_radio_failure() {
    if (recovering()) return false;
    phase_ = discovering() || phase_ == Phase::INITIALIZING_DISCOVERY ? Phase::RECOVERING_DISCOVERY : Phase::RECOVERING;
    return true;
  }
  bool retry() {
    if (!recovering()) return false;
    phase_ = phase_ == Phase::RECOVERING_DISCOVERY ? Phase::INITIALIZING_DISCOVERY : Phase::INITIALIZING;
    return true;
  }

 private:
  Phase phase_;
};

// Scheduling, TX completion, and reply acceptance are separate phases. The
// first matching ACK refreshes the lamp's queued state; only a later STATUS
// reply is publishable. Deadlines start at TX_DONE, never at queueing.
class StatusPoll {
  enum class Phase { IDLE, REFRESH_DELAY, REFRESH_TX, REFRESH_REPLY, READ_DELAY, READ_TX, READ_REPLY };

 public:
  enum class Reply { IGNORED, NEEDS_READ, STATE };
  static constexpr uint32_t SETTLE_MS = 500, REPLY_TIMEOUT_MS = 200;
  static constexpr uint8_t MAX_READ_ATTEMPTS = 3, FAILURE_THRESHOLD = 3;

  bool idle() const { return phase_ == Phase::IDLE; }
  bool transmitting() const { return phase_ == Phase::REFRESH_TX || phase_ == Phase::READ_TX; }
  bool waiting_reply() const { return phase_ == Phase::REFRESH_REPLY || phase_ == Phase::READ_REPLY; }
  bool reading() const {
    return phase_ == Phase::READ_DELAY || phase_ == Phase::READ_TX || phase_ == Phase::READ_REPLY;
  }
  bool due(uint32_t now) const {
    return (phase_ == Phase::REFRESH_DELAY || phase_ == Phase::READ_DELAY) &&
           static_cast<int32_t>(now - next_request_) >= 0;
  }
  uint8_t failures() const { return failures_; }
  void reset_failures() { failures_ = 0; }
  void cancel() { phase_ = Phase::IDLE; }
  void schedule(uint32_t now, uint32_t delay) {
    phase_ = Phase::REFRESH_DELAY;
    next_request_ = now + delay;
    read_attempts_ = 0;
  }
  bool begin_request(uint8_t pid) {
    if (phase_ == Phase::READ_DELAY) {
      ++read_attempts_;
      phase_ = Phase::READ_TX;
    } else if (phase_ == Phase::REFRESH_DELAY) {
      phase_ = Phase::REFRESH_TX;
    } else {
      return false;
    }
    request_pid_ = pid;
    return true;
  }
  bool on_tx_done(uint32_t now) {
    if (phase_ == Phase::REFRESH_TX)
      phase_ = Phase::REFRESH_REPLY;
    else if (phase_ == Phase::READ_TX)
      phase_ = Phase::READ_REPLY;
    else
      return false;
    reply_started_ = now;
    return true;
  }
  Reply on_reply(uint8_t pid, bool status, uint32_t now) {
    if (!waiting_reply() || pid != request_pid_) return Reply::IGNORED;
    if (phase_ == Phase::REFRESH_REPLY || (!status && read_attempts_ < MAX_READ_ATTEMPTS)) {
      phase_ = Phase::READ_DELAY;
      next_request_ = now + SETTLE_MS;
      return Reply::NEEDS_READ;
    }
    if (!status) return Reply::IGNORED;
    phase_ = Phase::IDLE;
    failures_ = 0;
    return Reply::STATE;
  }
  bool expire(uint32_t now) {
    if (!waiting_reply() || now - reply_started_ < REPLY_TIMEOUT_MS) return false;
    phase_ = Phase::IDLE;
    if (failures_ < FAILURE_THRESHOLD) ++failures_;
    return true;
  }

 private:
  Phase phase_{Phase::IDLE};
  uint32_t next_request_{0}, reply_started_{0};
  uint8_t request_pid_{0}, read_attempts_{0}, failures_{0};
};

// Publication is a callback scope, orthogonal to the radio lifecycle. Restore
// the previous guard value so nested publications cannot re-enable commands.
class ScopedPublication {
 public:
  explicit ScopedPublication(bool &publishing) : publishing_(publishing), previous_(std::exchange(publishing, true)) {}
  ~ScopedPublication() { publishing_ = previous_; }
  ScopedPublication(const ScopedPublication &) = delete;
  ScopedPublication &operator=(const ScopedPublication &) = delete;

 private:
  bool &publishing_;
  bool previous_;
};

}  // namespace esphome::halo2
