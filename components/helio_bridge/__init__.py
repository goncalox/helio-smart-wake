import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import ble_client, sensor, text_sensor, time
from esphome.const import CONF_ID

DEPENDENCIES = ["ble_client", "time"]
AUTO_LOAD = ["sensor", "text_sensor"]
ns = cg.esphome_ns.namespace("helio_bridge")
HelioBridge = ns.class_("HelioBridge", cg.Component, ble_client.BLEClientNode)

def auth_key(value):
    value = cv.string_strict(value).strip()
    if len(value) != 32 or any(c not in "0123456789abcdefABCDEF" for c in value):
        raise cv.Invalid("The Helio key must contain exactly 32 hexadecimal characters")
    return value

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(HelioBridge),
    cv.Required("auth_key"): cv.sensitive(auth_key),
    cv.Required("status"): text_sensor.text_sensor_schema(),
    cv.Required("alarm_status"): text_sensor.text_sensor_schema(),
    cv.Required("battery"): sensor.sensor_schema(unit_of_measurement="%", accuracy_decimals=0,
                                                device_class="battery", state_class="measurement"),
    cv.Required("time_id"): cv.use_id(time.RealTimeClock),
    cv.Required("diagnostic_status"): text_sensor.text_sensor_schema(entity_category="diagnostic"),
    cv.Required("sleep_status"): text_sensor.text_sensor_schema(),
    cv.Required("sleep_stage"): text_sensor.text_sensor_schema(),
    cv.Required("sleep_through"): text_sensor.text_sensor_schema(device_class="timestamp"),
    cv.Required("sleep_onset"): text_sensor.text_sensor_schema(device_class="timestamp"),
    cv.Required("sleep_sync"): text_sensor.text_sensor_schema(device_class="timestamp"),
    cv.Required("sleep_changed"): text_sensor.text_sensor_schema(device_class="timestamp"),
    cv.Required("sleep_age"): sensor.sensor_schema(unit_of_measurement="min", accuracy_decimals=1, state_class="measurement"),
    cv.Required("sleep_duration"): sensor.sensor_schema(unit_of_measurement="min", accuracy_decimals=0, state_class="measurement"),
    cv.Optional("night_duration"): sensor.sensor_schema(unit_of_measurement="min", accuracy_decimals=0, state_class="measurement"),
    cv.Required("sleep_records"): sensor.sensor_schema(accuracy_decimals=0),
    cv.Optional("sleep_score"): sensor.sensor_schema(accuracy_decimals=0, state_class="measurement"),
    cv.Optional("personal_score"): sensor.sensor_schema(accuracy_decimals=0, state_class="measurement"),
    cv.Optional("personal_score_coverage"): sensor.sensor_schema(unit_of_measurement="%", accuracy_decimals=0, entity_category="diagnostic"),
    cv.Optional("personal_score_status"): text_sensor.text_sensor_schema(entity_category="diagnostic"),
    cv.Optional("personal_score_details"): text_sensor.text_sensor_schema(entity_category="diagnostic"),
    cv.Optional("learning_status"): text_sensor.text_sensor_schema(entity_category="diagnostic"),
    cv.Required("model_stage"): text_sensor.text_sensor_schema(),
    cv.Required("smart_status"): text_sensor.text_sensor_schema(),
    cv.Required("smart_alarm"): text_sensor.text_sensor_schema(device_class="timestamp"),
    cv.Required("smart_target"): text_sensor.text_sensor_schema(device_class="timestamp"),
    cv.Required("smart_awake"): sensor.sensor_schema(unit_of_measurement="min", accuracy_decimals=0, state_class="measurement"),
    cv.Required("sleep_awake"): sensor.sensor_schema(unit_of_measurement="min", accuracy_decimals=0, state_class="measurement"),
}).extend(cv.COMPONENT_SCHEMA).extend(ble_client.BLE_CLIENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)
    cg.add(var.set_auth_key([int(config["auth_key"][i:i+2], 16) for i in range(0, 32, 2)]))
    cg.add(var.set_status_sensor(await text_sensor.new_text_sensor(config["status"])))
    cg.add(var.set_alarm_sensor(await text_sensor.new_text_sensor(config["alarm_status"])))
    cg.add(var.set_battery_sensor(await sensor.new_sensor(config["battery"])))
    cg.add(var.set_diagnostic_status(await text_sensor.new_text_sensor(config["diagnostic_status"])))
    cg.add(var.set_clock(await cg.get_variable(config["time_id"])))
    for key in ("sleep_status", "sleep_stage", "sleep_through", "sleep_onset", "sleep_sync", "sleep_changed"):
        cg.add(getattr(var, f"set_{key}")(await text_sensor.new_text_sensor(config[key])))
    for key in ("sleep_age", "sleep_duration", "sleep_records", "smart_awake", "sleep_awake"):
        cg.add(getattr(var, f"set_{key}")(await sensor.new_sensor(config[key])))
    for key in ("personal_score", "personal_score_coverage"):
        if key in config:
            cg.add(getattr(var, f"set_{key}")(await sensor.new_sensor(config[key])))
    for key in ("personal_score_status", "personal_score_details"):
        if key in config:
            cg.add(getattr(var, f"set_{key}")(await text_sensor.new_text_sensor(config[key])))
    if "learning_status" in config:
        cg.add(var.set_learning_status(await text_sensor.new_text_sensor(config["learning_status"])))
    if "sleep_score" in config:
        cg.add(var.set_sleep_score(await sensor.new_sensor(config["sleep_score"])))
    if "night_duration" in config:
        cg.add(var.set_night_duration(await sensor.new_sensor(config["night_duration"])))
    for key in ("model_stage", "smart_status", "smart_alarm", "smart_target"):
        cg.add(getattr(var, f"set_{key}")(await text_sensor.new_text_sensor(config[key])))
