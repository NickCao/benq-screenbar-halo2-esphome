import asyncio
import re
from time import monotonic

import pytest

pytestmark = [pytest.mark.hardware, pytest.mark.asyncio(loop_scope="module")]


async def test_independent_brightness_and_shared_temperature(lamp):
    since = monotonic()
    lamp.light("Back lamp", state=True, brightness=.62, color_temperature=1_000_000 / 4500)
    lamp.light("Front lamp", state=True, brightness=.37)
    await lamp.wait_readback(
        since, power=True, front=True, back=True, front_brightness=37,
        back_brightness=62, temperature=4500,
    )
    for name in ("Front lamp", "Back lamp"):
        await lamp.wait_light_state(name, color_temperature=1_000_000 / 4500)


@pytest.mark.parametrize("temperature", [2700, 3925, 6500])
async def test_color_temperature_output_conversion(lamp, temperature):
    since = monotonic()
    lamp.light("Front lamp", state=True, brightness=.43, color_temperature=1_000_000 / temperature)
    await lamp.wait_readback(
        since, power=True, front=True, back=True, front_brightness=43,
        back_brightness=62, temperature=temperature,
    )
    for name in ("Front lamp", "Back lamp"):
        await lamp.wait_light_state(name, color_temperature=1_000_000 / temperature)

    since = monotonic()
    lamp.light("Front lamp", brightness=.37, color_temperature=1_000_000 / 4500)
    await lamp.wait_readback(since, front_brightness=37, temperature=4500)


async def test_temperature_rounding_preserves_peer_transition(lamp):
    since = monotonic()
    lamp.light("Front lamp", state=True, brightness=.37, color_temperature=1_000_000 / 4500)
    lamp.light("Back lamp", state=True, brightness=.62, color_temperature=1_000_000 / 4500)
    await lamp.wait_readback(
        since, power=True, front=True, back=True, front_brightness=37,
        back_brightness=62, temperature=4500,
    )
    transition_length = 4
    started = monotonic()
    lamp.light("Back lamp", brightness=.20, transition_length=transition_length)
    await asyncio.sleep(.25)
    # This rounds to the existing shared setting. Correcting the front entity
    # must not mirror an unchanged temperature and end the rear's fade early.
    lamp.light("Front lamp", color_temperature=1_000_000 / 4503)
    await lamp.wait_light_state("Front lamp", color_temperature=1_000_000 / 4500)
    await lamp.wait_readback(
        started, power=True, front=True, back=True, front_brightness=37,
        back_brightness=20, temperature=4500,
    )
    # Fresh readback is deferred while ESPHome's transition is active.
    assert monotonic() - started >= transition_length - .5

    since = monotonic()
    lamp.light("Back lamp", brightness=.62)
    await lamp.wait_readback(since, back_brightness=62, temperature=4500)


@pytest.mark.parametrize("on,brightness", [(False, .62), (True, .20)], ids=["off", "dim"])
async def test_shared_temperature_reconciles_a_peer_fade(lamp, on, brightness):
    lamp.light("Back lamp", state=on, brightness=brightness, transition_length=2)
    await asyncio.sleep(.25)
    since = monotonic()
    lamp.light("Front lamp", color_temperature=1_000_000 / 4725)
    await lamp.wait_readback(
        since, power=True, front=True, back=on, front_brightness=37,
        temperature=4725, **({"back_brightness": round(brightness * 100)} if on else {}),
    )
    await lamp.wait_light_state(
        "Back lamp", state=on, brightness=brightness, color_temperature=1_000_000 / 4725,
    )
    if not on:
        # Unselected sections ignore the packet's brightness. A plain ON
        # must still apply the retained level after receiving that readback.
        since = monotonic()
        lamp.light("Back lamp", state=True)
        await lamp.wait_readback(since, back=True, back_brightness=62, temperature=4725)

    since = monotonic()
    lamp.light("Back lamp", state=True, brightness=.62, color_temperature=1_000_000 / 4500)
    await lamp.wait_readback(since, power=True, front=True, back=True, back_brightness=62, temperature=4500)


@pytest.mark.parametrize("first,second", [("Front lamp", "Back lamp"), ("Back lamp", "Front lamp")])
async def test_grouped_on_off_in_either_order(lamp, first, second):
    since = monotonic()
    lamp.light(first, state=False)
    lamp.light(second, state=False)
    await lamp.wait_readback(since, power=False, front_brightness=37, back_brightness=62)

    since = monotonic()
    lamp.light(first, state=True)
    lamp.light(second, state=True)
    await lamp.wait_readback(since, power=True, front=True, back=True, front_brightness=37, back_brightness=62)


async def test_section_off_preserves_peer(lamp):
    since = monotonic()
    lamp.light("Front lamp", state=False)
    await lamp.wait_readback(since, power=True, front=False, back=True, front_brightness=37, back_brightness=62)
    since = monotonic()
    lamp.light("Front lamp", state=True)
    await lamp.wait_readback(since, power=True, front=True, back=True, front_brightness=37, back_brightness=62)


@pytest.mark.parametrize("wait_for_off", [True, False], ids=["after-readback", "before-poll"])
async def test_zero_brightness_then_plain_on(lamp, wait_for_off):
    since = monotonic()
    lamp.light("Front lamp", brightness=0)
    if wait_for_off:
        await lamp.wait_readback(since, power=True, front=False, back=True, front_brightness=37)
    else:
        # Give the API command time to be applied, without waiting for a poll
        # that could hide lost brightness in the entity adapter.
        await asyncio.sleep(.15)
    since = monotonic()
    lamp.light("Front lamp", state=True)
    await lamp.wait_readback(since, power=True, front=True, back=True, front_brightness=37, back_brightness=62)
    await lamp.wait_light_state("Front lamp", state=True, brightness=.37)


async def test_combined_power_presence_and_timeout_changes(lamp):
    since = monotonic()
    lamp.light("Front lamp", state=False)
    lamp.light("Back lamp", state=False)
    lamp.select("10 minutes")
    await lamp.wait_readback(since, power=False, ultrasonic=True, timeout=10)

    since = monotonic()
    lamp.light("Back lamp", state=True)
    lamp.light("Front lamp", state=True)
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
    lamp.light("Front lamp", state=False)
    lamp.light("Back lamp", state=False)
    await lamp.wait_readback(since, power=False)

    lamp.radio_messages.clear()
    since = monotonic()
    lamp.button("Resend current state")
    await lamp.wait_readback(
        since, power=False, front_brightness=37, back_brightness=62,
        temperature=4500, ultrasonic=False, timeout=3,
    )
    commands = [match.group(1) for message in lamp.radio_messages
                if (match := re.search(r"TX command 0x(02|03|05)", message))]
    assert commands == ["02", "03", "05"]
