from esphome import pins
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button, light, select, spi, text_sensor
from esphome.const import (
    CONF_BUSY_PIN,
    CONF_DATA_RATE,
    CONF_DEFAULT_TRANSITION_LENGTH,
    CONF_GAMMA_CORRECT,
    CONF_ID,
    CONF_IRQ_PIN,
    CONF_RESET_PIN,
    CONF_UPDATE_INTERVAL,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
)

AUTO_LOAD = ["button", "light", "select", "text_sensor"]
DEPENDENCIES = ["esp32", "spi"]

halo2_ns = cg.esphome_ns.namespace("halo2")
Halo2 = halo2_ns.class_("Halo2", cg.PollingComponent)
Halo2Light = halo2_ns.class_("Halo2Light", light.LightOutput)
Section = halo2_ns.enum("Section", is_class=True)
Halo2UltrasonicSelect = halo2_ns.class_("Halo2UltrasonicSelect", select.Select)
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


BASE_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(Halo2),
        cv.Optional("auto_discover", default=True): cv.boolean,
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
        cv.Optional("status_poll_interval", default="5s"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(min=cv.TimePeriod(milliseconds=1000)),
        ),
        cv.Optional("command_debounce", default="1s"): cv.positive_time_period_milliseconds,
        cv.Optional("frequency_deviation_hz", default=160000): cv.int_range(
            min=1, max=170999
        ),
        cv.Optional("pulse_shape", default=0x09): cv.one_of(
            0x00, 0x08, 0x09, 0x0A, 0x0B, int=True
        ),
        cv.Required("front_light"): LIGHT_SCHEMA,
        cv.Required("back_light"): LIGHT_SCHEMA,
        cv.Required("ultrasonic"): select.select_schema(
            Halo2UltrasonicSelect,
            entity_category=ENTITY_CATEGORY_CONFIG,
            icon="mdi:motion-sensor",
        ),
        cv.Required("radio_status"): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    }
).extend(cv.polling_component_schema("50ms"))

CONFIG_SCHEMA = cv.All(
    BASE_SCHEMA.extend(spi.spi_device_schema(cs_pin_required=True)).extend(
        {
            cv.Required(CONF_RESET_PIN): pins.internal_gpio_output_pin_schema,
            cv.Required(CONF_BUSY_PIN): pins.internal_gpio_input_pin_schema,
            cv.Required(CONF_IRQ_PIN): pins.internal_gpio_input_pin_schema,
            # Keep the validated transfer timing and the LR1121's required mode.
            cv.Optional(CONF_DATA_RATE, default="1MHz"): cv.All(
                spi.SPI_DATA_RATE_SCHEMA, cv.one_of(1000000)
            ),
            cv.Optional(spi.CONF_SPI_MODE, default="MODE0"): cv.enum(
                {"MODE0": spi.SPIMode.MODE0, 0: spi.SPIMode.MODE0, "0": spi.SPIMode.MODE0},
                upper=True,
            ),
        }
    ),
    cv.only_on_esp32,
)

FINAL_VALIDATE_SCHEMA = spi.final_validate_device_schema(
    "halo2", require_mosi=True, require_miso=True
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    # Preserve the existing YAML names: update_interval services commands/RX,
    # while the PollingComponent interval schedules lamp status queries.
    await cg.register_component(
        var, {**config, CONF_UPDATE_INTERVAL: config["status_poll_interval"]}
    )
    radio = var.get_radio()
    await spi.register_spi_device(radio, config)
    cg.add(radio.set_reset_pin(await cg.gpio_pin_expression(config[CONF_RESET_PIN])))
    cg.add(radio.set_busy_pin(await cg.gpio_pin_expression(config[CONF_BUSY_PIN])))
    cg.add(radio.set_irq_pin(await cg.gpio_pin_expression(config[CONF_IRQ_PIN])))
    cg.add(var.set_command_debounce(config["command_debounce"]))
    cg.add(var.set_frequency_deviation(config["frequency_deviation_hz"]))
    cg.add(var.set_pulse_shape(config["pulse_shape"]))
    cg.add(var.set_auto_discover(config["auto_discover"]))
    if "radio_address" in config:
        cg.add(var.set_radio_address(cg.ArrayInitializer(*config["radio_address"])))
    if "radio_channel" in config:
        cg.add(var.set_radio_channel(config["radio_channel"]))
    await light.new_light(config["front_light"], var, Section.FRONT)
    await light.new_light(config["back_light"], var, Section.BACK)
    ultrasonic = await select.new_select(
        config["ultrasonic"], var,
        options=["Disabled", "3 minutes", "5 minutes", "10 minutes", "15 minutes"],
    )
    cg.add(var.set_ultrasonic_select(ultrasonic))
    status = await text_sensor.new_text_sensor(config["radio_status"])
    cg.add(var.set_radio_status(status))
    if "radio_address_sensor" in config:
        address = await text_sensor.new_text_sensor(config["radio_address_sensor"])
        cg.add(var.set_radio_address_sensor(address))
    if "discover_button" in config:
        await button.new_button(config["discover_button"], var)
    if "auto_button" in config:
        await button.new_button(config["auto_button"], var)
    cg.add(var.set_processing_interval(config[CONF_UPDATE_INTERVAL]))
