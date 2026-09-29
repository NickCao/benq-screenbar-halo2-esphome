"""Native API commands verified by fresh lamp replies in the bridge's DEBUG logs."""

import asyncio
import logging
import re
from collections import deque
from dataclasses import dataclass
from time import monotonic

from aioesphomeapi import APIClient
from aioesphomeapi.model import LightInfo, LogLevel, SelectInfo

LOGGER = logging.getLogger(__name__)
STATUS_PATTERN = re.compile(
    r"Lamp status: power (ON|OFF), mode (\d)/(\d), front (\d+)%, "
    r"back (\d+)%, (\d+) K, ultrasonic (ON|OFF), timeout (\d+) min"
)


@dataclass(frozen=True)
class LampReadback:
    power: bool
    front: bool
    back: bool
    front_brightness: int
    back_brightness: int
    temperature: int
    ultrasonic: bool
    timeout: int


class Lamp:
    def __init__(self, host, encryption_key):
        self.api = APIClient(host, 6053, noise_psk=encryption_key, client_info="HALO 2 hardware tests")
        self.entities = {}
        self.states = {}
        self.readbacks = deque(maxlen=100)
        self.radio_messages = deque(maxlen=50)
        self.changed = asyncio.Event()
        self.unsubscribe_logs = None

    async def connect(self):
        await self.api.connect(login=True)
        infos, _ = await self.api.list_entities_services()
        self.entities = {info.name: info for info in infos}
        self.api.subscribe_states(self.on_state)
        self.unsubscribe_logs = self.api.subscribe_logs(self.on_log, log_level=LogLevel.LOG_LEVEL_DEBUG)

    async def disconnect(self):
        if self.unsubscribe_logs is not None:
            self.unsubscribe_logs()
        await self.api.disconnect()

    def on_state(self, state):
        self.states[state.key] = state
        self.changed.set()

    def on_log(self, response):
        message = response.message.decode(errors="replace")
        match = STATUS_PATTERN.search(message)
        if match:
            power, front, back, fb, bb, temperature, ultrasonic, timeout = match.groups()
            state = LampReadback(
                power == "ON", front == "1", back == "1", int(fb), int(bb),
                int(temperature), ultrasonic == "ON", int(timeout),
            )
            self.readbacks.append((monotonic(), state))
            LOGGER.info("Lamp readback: %s", state)
            self.changed.set()
        elif re.search(r"\[(?:halo2|lr1121):", message):
            clean = re.sub(r"\x1b\[[0-9;]*m", "", message)
            self.radio_messages.append(clean)
            LOGGER.debug("%s", clean)

    def entity(self, name, kind):
        matches = [info for label, info in self.entities.items()
                   if isinstance(info, kind) and label.lower().endswith(name.lower())]
        if len(matches) != 1:
            raise AssertionError(f"Expected one {kind.__name__} named {name!r}; found {len(matches)}")
        return matches[0]

    def light(self, name, **values):
        self.api.light_command(self.entity(name, LightInfo).key, transition_length=0, **values)

    def select(self, option):
        self.api.select_command(self.entity("Ultrasonic sensor", SelectInfo).key, option)

    async def wait_readback(self, since, *, wait_timeout=25, **expected):
        """Wait for a lamp observation made after the commands, allowing RF retries."""
        deadline = monotonic() + wait_timeout
        while monotonic() < deadline:
            self.changed.clear()
            for received_at, state in reversed(self.readbacks):
                if received_at > since and all(getattr(state, key) == value for key, value in expected.items()):
                    return state
            try:
                await asyncio.wait_for(self.changed.wait(), max(.01, min(1, deadline - monotonic())))
            except TimeoutError:
                pass
        last = self.readbacks[-1][1] if self.readbacks else None
        raise AssertionError(
            f"No fresh lamp readback matching {expected} within {wait_timeout}s. "
            f"Last readback: {last}. Recent radio messages: {list(self.radio_messages)}"
        )

    async def wait_light_state(self, name, *, timeout=3, **expected):
        key = self.entity(name, LightInfo).key
        deadline = monotonic() + timeout
        while monotonic() < deadline:
            self.changed.clear()
            state = self.states.get(key)
            if state is not None and all(abs(getattr(state, field) - value) < .001
                                         for field, value in expected.items()):
                return state
            try:
                await asyncio.wait_for(self.changed.wait(), max(.01, deadline - monotonic()))
            except TimeoutError:
                pass
        raise AssertionError(f"{name}: expected {expected}, last API state {self.states.get(key)}")
