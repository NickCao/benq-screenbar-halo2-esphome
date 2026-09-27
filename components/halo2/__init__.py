import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button, light, switch, text_sensor
from esphome.const import (
    CONF_DEFAULT_TRANSITION_LENGTH,
    CONF_GAMMA_CORRECT,
    CONF_ID,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
)

AUTO_LOAD = ["button", "light", "switch", "text_sensor"]
DEPENDENCIES = ["esp32"]

halo2_ns = cg.esphome_ns.namespace("halo2")
Halo2 = halo2_ns.class_("Halo2", cg.PollingComponent)
Halo2Light = halo2_ns.class_("Halo2Light", light.LightOutput)
Halo2UltrasonicSwitch = halo2_ns.class_("Halo2UltrasonicSwitch", switch.Switch)
Halo2DiscoverButton = halo2_ns.class_("Halo2DiscoverButton", button.Button)
Halo2AutoButton = halo2_ns.class_("Halo2AutoButton", button.Button)

LIGHT_SCHEMA = light.light_schema(
    Halo2Light,
    light.LightType.BRIGHTNESS_ONLY,
    default_restore_mode="RESTORE_DEFAULT_OFF",
).extend(
    {
        # The lamp already interprets brightness as a percentage.
        cv.Optional(CONF_GAMMA_CORRECT, default=1.0): cv.All(
            cv.float_, cv.one_of(1.0)
        ),
        cv.Optional(
            CONF_DEFAULT_TRANSITION_LENGTH, default="0s"
        ): cv.positive_time_period_milliseconds,
    }
)


def _validate_radio_options(config):
    if config["radio"] == "BM5602":
        for key in (
            "auto_discover",
            "radio_address",
            "radio_channel",
            "radio_address_sensor",
            "discover_button",
            "status_poll_interval",
        ):
            if key in config:
                raise cv.Invalid(f"{key} is currently supported only with LR1121", path=[key])
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(Halo2),
            cv.Required("radio"): cv.one_of("LR1121", "BM5602", upper=True),
            cv.Optional("auto_discover"): cv.boolean,
            cv.Optional("radio_address"): cv.All(
                cv.ensure_list(cv.hex_uint8_t), cv.Length(min=4, max=4)
            ),
            cv.Optional("radio_channel"): cv.one_of(5, 46, 75, int=True),
            cv.Optional("radio_address_sensor"): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional("discover_button"): button.button_schema(
                Halo2DiscoverButton, entity_category=ENTITY_CATEGORY_CONFIG,
                icon="mdi:radar",
            ),
            cv.Optional("auto_button"): button.button_schema(
                Halo2AutoButton, icon="mdi:brightness-auto",
            ),
            cv.Optional("status_poll_interval"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(min=cv.TimePeriod(milliseconds=1000)),
            ),
            cv.Optional("frequency_deviation_hz", default=160000): cv.int_range(
                min=1, max=170999
            ),
            cv.Optional("pulse_shape", default=0x09): cv.one_of(
                0x00, 0x08, 0x09, 0x0A, 0x0B, int=True
            ),
            cv.Required("front_light"): LIGHT_SCHEMA,
            cv.Required("back_light"): LIGHT_SCHEMA,
            cv.Required("ultrasonic"): switch.switch_schema(
                Halo2UltrasonicSwitch,
                block_inverted=True,
                default_restore_mode="RESTORE_DEFAULT_OFF",
                icon="mdi:motion-sensor",
            ),
            cv.Required("radio_status"): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
        }
    ).extend(cv.polling_component_schema("50ms")),
    cv.only_on_esp32,
    _validate_radio_options,
)


async def to_code(config):
    cg.add_define(f"USE_HALO2_{config['radio']}")
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_frequency_deviation(config["frequency_deviation_hz"]))
    cg.add(var.set_pulse_shape(config["pulse_shape"]))
    cg.add(var.set_auto_discover(config.get("auto_discover", config["radio"] == "LR1121")))
    if "radio_address" in config:
        cg.add(var.set_radio_address(cg.ArrayInitializer(*config["radio_address"])))
    if "radio_channel" in config:
        cg.add(var.set_radio_channel(config["radio_channel"]))
    await light.new_light(config["front_light"], var, True)
    await light.new_light(config["back_light"], var, False)
    ultrasonic = await switch.new_switch(config["ultrasonic"], var)
    cg.add(var.set_ultrasonic_switch(ultrasonic))
    status = await text_sensor.new_text_sensor(config["radio_status"])
    cg.add(var.set_radio_status(status))
    if "radio_address_sensor" in config:
        address = await text_sensor.new_text_sensor(config["radio_address_sensor"])
        cg.add(var.set_radio_address_sensor(address))
    if "discover_button" in config:
        await button.new_button(config["discover_button"], var)
    if "auto_button" in config:
        await button.new_button(config["auto_button"], var)
    if config["radio"] == "LR1121":
        cg.add(var.set_status_poll_interval(config.get("status_poll_interval", 5000)))
