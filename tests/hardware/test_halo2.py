import asyncio
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
