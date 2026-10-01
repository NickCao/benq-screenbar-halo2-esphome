# Native regression tests

Run all native tests from the repository root:

    ./scripts/test

Only a C++20 compiler is required. Set CXX to its path if c++ is unavailable.
The runner builds into a temporary directory and removes its binaries on exit.
CI runs this same command.

The model tests exercise lamp settings, command planning, lifecycle, and polling
in isolation. The coordinator suite compiles the production halo2.cpp and
halo2_light.cpp, including the real LR1121Radio and protocol codec. It drives
setup(), loop(), update(), and entity commands without accessing the
coordinator's protected state.

## Protocol coverage

- All four original-controller Favorite save/recall captures decode into their
  expected settings, command codes, and request PCFs, and support address discovery.
- Favorite frames retain CRC, address, framing, and payload validation. Replies
  are accepted only when explicitly enabled.
- Legacy `0x00..0x05` commands remain accepted; `0x06`, `0x0B`, and other
  unsupported command codes remain rejected.

## Coordinator coverage

- Boot requires a fresh baseline; reflecting received state creates no commands.
- Commands arriving during either polling transmission take precedence over its
  old reply, without inheriting a command cooldown from a polling completion.
- Power-on sends settings before power. New intent during that batch waits for
  both packets and the cooldown, and uses the latest requested state.
- Failure between the two power-on packets discards both interrupted and pending
  work. Recovery reads a new baseline without replaying commands.
- Discovery during a command or status transmission ignores the old completion,
  blocks local commands, and resumes polling on the newly discovered address.
  This includes discovery between the two frames of a power-on batch.
- Recovery during discovery restarts the scan with a new dwell timer.
- Missing replies retain entity state, raise a warning after three failed cycles,
  and clear the failure count after a successful read.
- Controller requests update entities without echoing a radio command.
- Captured Favorite requests publish settings without echo, then reconcile
  brightness, temperature, and presence through fresh lamp polling.
- Matching Favorite replies can acknowledge refresh and trigger read retries;
  only a fresh status reply publishes lamp state.
- Wrong-PID replies and queued non-status replies cannot publish lamp state.
- Polling waits for transitions, including the interval before the first sample.
- Temperature rounding preserves a peer fade; a changed shared temperature
  reconciles the peer's final brightness and on/off target.
- Deferred writes capture the final transition target installed after
  update_state().

Assertions check transmitted frames, published entities, timing, and recovery
effects. They do not assert the private phases of the coordinator's state
machines, so those implementations can be refactored behind the same behavior.

## Test boundary

support/fake_lr1121.h implements the chip's SPI responses, FIFO, and IRQ bits.
Tests explicitly finish transmissions, inject received bytes, or raise faults.
It does not generate successful acknowledgements automatically or implement a
second copy of the bridge's scheduling policy.

The support/esphome/ headers provide the small part of ESPHome needed by these
sources: GPIO/SPI interfaces, an in-memory preference store, entity publication,
and named timers driven by a virtual clock. Periodic update() calls are explicit.
The light shim preserves immediate update_state() and deferred write_state()
callbacks. Tests inject selected transition samples and the final target in
ESPHome's callback order; the shim does not reproduce fade interpolation,
effects, configuration validation, or ESPHome's full scheduler.

This suite protects component behavior under those framework contracts.
Firmware compilation still checks compatibility with real ESPHome, and the
opt-in hardware suite remains necessary to verify real transitions, RF timing,
and the lamp's physical behavior.
