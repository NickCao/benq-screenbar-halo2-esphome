from pathlib import Path
from time import monotonic

import pytest
import pytest_asyncio
import yaml

from .lamp import Lamp


def pytest_addoption(parser):
    parser.addoption("--halo2-device", help="Opt in to operating this bridge and its lamp (hostname or IP)")
    parser.addoption(
        "--halo2-secrets", type=Path, default=Path(__file__).resolve().parents[2] / "secrets.yaml",
        help="YAML file containing api_encryption_key (default: repository secrets.yaml)",
    )


@pytest_asyncio.fixture(scope="module", loop_scope="module")
async def lamp(pytestconfig):
    host = pytestconfig.getoption("--halo2-device")
    if not host:
        pytest.skip("Specify --halo2-device to run tests that operate a real lamp")
    secrets = yaml.safe_load(pytestconfig.getoption("--halo2-secrets").read_text())
    device = Lamp(host, secrets["api_encryption_key"])
    try:
        await device.connect()
        await device.wait_readback(monotonic())
        yield device
    finally:
        await device.disconnect()


@pytest_asyncio.fixture(autouse=True, loop_scope="module")
async def baseline_profile(lamp):
    since = monotonic()
    lamp.select("Disabled")
    lamp.sections("Both")
    lamp.light("ScreenBar", state=True, color_temperature=1_000_000 / 4500)
    lamp.number("Front brightness", 37)
    lamp.number("Back brightness", 62)
    await lamp.wait_readback(
        since, power=True, front=True, back=True, front_brightness=37,
        back_brightness=62, temperature=4500, ultrasonic=False,
    )
