import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import light, switch, text_sensor
from esphome.const import (
    CONF_DEFAULT_TRANSITION_LENGTH,
    CONF_GAMMA_CORRECT,
    CONF_ID,
    ENTITY_CATEGORY_DIAGNOSTIC,
)

AUTO_LOAD = ["light", "switch", "text_sensor"]
DEPENDENCIES = ["esp32"]

halo2_ns = cg.esphome_ns.namespace("halo2")
Halo2 = halo2_ns.class_("Halo2", cg.PollingComponent)
Halo2Light = halo2_ns.class_("Halo2Light", light.LightOutput)
Halo2Switch = halo2_ns.class_("Halo2Switch", switch.Switch)

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

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(Halo2),
            cv.Required("radio"): cv.one_of("LR1121", "BM5602", upper=True),
            cv.Optional("frequency_deviation_hz", default=160000): cv.int_range(
                min=1, max=170999
            ),
            cv.Optional("pulse_shape", default=0x09): cv.one_of(
                0x00, 0x08, 0x09, 0x0A, 0x0B, int=True
            ),
            cv.Required("front_light"): LIGHT_SCHEMA,
            cv.Required("back_light"): LIGHT_SCHEMA,
            cv.Required("power"): switch.switch_schema(
                Halo2Switch,
                block_inverted=True,
                default_restore_mode="DISABLED",
                icon="mdi:power",
            ),
            cv.Required("ultrasonic"): switch.switch_schema(
                Halo2Switch,
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
)


async def to_code(config):
    cg.add_define(f"USE_HALO2_{config['radio']}")
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_frequency_deviation(config["frequency_deviation_hz"]))
    cg.add(var.set_pulse_shape(config["pulse_shape"]))
    await light.new_light(config["front_light"], var, True)
    await light.new_light(config["back_light"], var, False)
    power = await switch.new_switch(config["power"], var, True)
    cg.add(var.set_power_switch(power))
    ultrasonic = await switch.new_switch(config["ultrasonic"], var, False)
    cg.add(var.set_ultrasonic_switch(ultrasonic))
    status = await text_sensor.new_text_sensor(config["radio_status"])
    cg.add(var.set_radio_status(status))
