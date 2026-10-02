import asyncio
import re
from time import monotonic

import pytest

pytestmark = [pytest.mark.hardware, pytest.mark.asyncio(loop_scope="module")]


async def test_independent_brightness_and_shared_temperature(lamp):
    since = monotonic()
    lamp.number("Front brightness", 43)
    lamp.number("Back brightness", 83)
    lamp.light("ScreenBar", color_temperature=1_000_000 / 4700)
    await lamp.wait_readback(
        since, power=True, front=True, back=True, front_brightness=43,
        back_brightness=83, temperature=4700,
    )
    await lamp.wait_light_state("ScreenBar", state=True, brightness=.83, color_temperature=1_000_000 / 4700)
    await lamp.wait_number_state("Front brightness", 43)
    await lamp.wait_number_state("Back brightness", 83)


@pytest.mark.parametrize("temperature,expected", [(2700, 2700), (3925, 3925), (6500, 6500), (4503, 4500)])
async def test_color_temperature_conversion(lamp, temperature, expected):
    since = monotonic()
    lamp.light("ScreenBar", color_temperature=1_000_000 / temperature)
    await lamp.wait_readback(
        since, power=True, front=True, back=True, front_brightness=37,
        back_brightness=62, temperature=expected,
    )
    await lamp.wait_light_state("ScreenBar", color_temperature=1_000_000 / expected)


async def test_native_master_brightness_dims_proportionally(lamp):
    since = monotonic()
    lamp.light("ScreenBar", state=True, brightness=.31)
    await lamp.wait_readback(since, power=True, front=True, back=True, front_brightness=19, back_brightness=31)
    await lamp.wait_light_state("ScreenBar", brightness=.31)
    await lamp.wait_number_state("Front brightness", 19)
    await lamp.wait_number_state("Back brightness", 31)


@pytest.mark.parametrize("mode,front,back,brightness", [
    ("Front", True, False, .37), ("Back", False, True, .62), ("Both", True, True, .62),
])
async def test_section_selection_preserves_stored_levels(lamp, mode, front, back, brightness):
    since = monotonic()
    lamp.sections(mode)
    await lamp.wait_readback(
        since, power=True, front=front, back=back, front_brightness=37, back_brightness=62,
    )
    await lamp.wait_light_state("ScreenBar", state=True, brightness=brightness)
    await lamp.wait_number_state("Front brightness", 37)
    await lamp.wait_number_state("Back brightness", 62)


async def test_inactive_section_shows_its_stored_level(lamp):
    since = monotonic()
    lamp.sections("Back")
    await lamp.wait_readback(since, power=True, front=False, back=True, front_brightness=37, back_brightness=62)
    since = monotonic()
    lamp.number("Front brightness", 80)
    await lamp.wait_number_state("Front brightness", 37)
    await lamp.wait_readback(since, front=False, back=True, front_brightness=37, back_brightness=62)


async def test_settings_while_off_preserve_global_power(lamp):
    since = monotonic()
    lamp.light("ScreenBar", state=False)
    await lamp.wait_readback(since, power=False, front=True, back=True, front_brightness=37, back_brightness=62)
    since = monotonic()
    lamp.sections("Front")
    lamp.number("Front brightness", 55)
    await lamp.wait_readback(since, power=False, front=True, back=False, front_brightness=55, back_brightness=62)
    await lamp.wait_light_state("ScreenBar", state=False, brightness=.55)
    since = monotonic()
    lamp.light("ScreenBar", state=True)
    await lamp.wait_readback(since, power=True, front=True, back=False, front_brightness=55, back_brightness=62)


@pytest.mark.parametrize("mode,front,back", [("Front", True, False), ("Back", False, True), ("Both", True, True)])
async def test_master_off_with_presence_retains_wake_settings(lamp, mode, front, back):
    since = monotonic()
    lamp.sections(mode)
    lamp.select("5 minutes")
    await lamp.wait_readback(since, power=True, front=front, back=back, ultrasonic=True, timeout=5)
    since = monotonic()
    lamp.light("ScreenBar", state=False)
    await lamp.wait_readback(
        since, power=False, front=front, back=back, front_brightness=37,
        back_brightness=62, temperature=4500, ultrasonic=True, timeout=5,
    )
    await lamp.wait_number_state("Front brightness", 37)
    await lamp.wait_number_state("Back brightness", 62)


@pytest.mark.parametrize("wait_for_off", [True, False], ids=["after-readback", "before-poll"])
async def test_zero_master_brightness_then_plain_on(lamp, wait_for_off):
    since = monotonic()
    lamp.light("ScreenBar", brightness=0)
    if wait_for_off:
        await lamp.wait_readback(since, power=False, front=True, back=True, front_brightness=37, back_brightness=62)
    else:
        await asyncio.sleep(.15)
    since = monotonic()
    lamp.light("ScreenBar", state=True)
    await lamp.wait_readback(since, power=True, front=True, back=True, front_brightness=37, back_brightness=62)
    await lamp.wait_light_state("ScreenBar", state=True, brightness=.62)


async def test_off_transition_retains_both_levels(lamp):
    started = monotonic()
    lamp.light("ScreenBar", state=False, transition_length=2)
    await lamp.wait_readback(
        started, power=False, front=True, back=True, front_brightness=37, back_brightness=62,
    )
    assert monotonic() - started >= 1.5
    await lamp.wait_light_state("ScreenBar", state=False, brightness=.62)


async def test_dimming_transition_uses_the_original_ratio(lamp):
    started = monotonic()
    lamp.light("ScreenBar", state=True, brightness=.31, transition_length=4)
    await lamp.wait_readback(started, power=True, front=True, back=True, front_brightness=19, back_brightness=31)
    assert monotonic() - started >= 3.5


async def test_section_brightness_finishes_a_master_transition(lamp):
    lamp.light("ScreenBar", brightness=.20, transition_length=4)
    await asyncio.sleep(.25)
    since = monotonic()
    lamp.number("Front brightness", 55)
    await lamp.wait_readback(since, power=True, front=True, back=True, front_brightness=55, back_brightness=20)
    await lamp.wait_light_state("ScreenBar", state=True, brightness=.55)


async def test_combined_power_presence_and_timeout_changes(lamp):
    since = monotonic()
    lamp.light("ScreenBar", state=False)
    lamp.select("10 minutes")
    await lamp.wait_readback(
        since, power=False, front=True, back=True, front_brightness=37, back_brightness=62, ultrasonic=True, timeout=10,
    )
    since = monotonic()
    lamp.light("ScreenBar", state=True)
    lamp.select("Disabled")
    await lamp.wait_readback(
        since, power=True, front=True, back=True, front_brightness=37,
        back_brightness=62, ultrasonic=False, timeout=10,
    )


@pytest.mark.parametrize("minutes", [5, 10, 15, 3])
async def test_presence_timeout_and_disable_retention(lamp, minutes):
    try:
        since = monotonic()
        lamp.select(f"{minutes} minutes")
        await lamp.wait_readback(since, ultrasonic=True, timeout=minutes)
    finally:
        since = monotonic()
        lamp.select("Disabled")
        disabled = await lamp.wait_readback(since, ultrasonic=False)
    assert disabled.timeout == minutes


async def test_resend_while_off_reapplies_settings_and_timeout(lamp):
    since = monotonic()
    lamp.select("5 minutes")
    await lamp.wait_readback(since, ultrasonic=True, timeout=5)
    since = monotonic()
    lamp.light("ScreenBar", state=False)
    lamp.select("Disabled")
    await lamp.wait_readback(since, power=False, ultrasonic=False, timeout=5)
    lamp.radio_messages.clear()
    since = monotonic()
    lamp.button("Resend current state")
    await lamp.wait_readback(
        since, power=False, front=True, back=True, front_brightness=37, back_brightness=62,
        temperature=4500, ultrasonic=False, timeout=5,
    )
    commands = [match.group(1) for message in lamp.radio_messages
                if (match := re.search(r"TX command 0x(02|03|05)", message))]
    assert commands == ["02", "03", "05"]
