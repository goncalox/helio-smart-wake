"""Sleep data/models and reliable strap transport; automations own wake decisions."""
from datetime import datetime
import voluptuous as vol
from homeassistant.const import Platform
from homeassistant.core import SupportsResponse
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers import config_validation as cv
from homeassistant.util import dt as dt_util
from .controller import Controller

DOMAIN = "helio_smart_wake"
PLATFORMS = [Platform.SENSOR, Platform.BINARY_SENSOR, Platform.NUMBER, Platform.SWITCH]

async def async_setup(hass, config):
    def controller():
        controllers = list(hass.data.get(DOMAIN, {}).values())
        if len(controllers) != 1:
            raise HomeAssistantError("Expected one Helio bridge")
        return controllers[0]
    async def activate(call):
        return await controller().activate()
    async def set_alarm(call):
        when = call.data["when"]
        if isinstance(when, datetime):
            if when.tzinfo is None:
                when = when.replace(tzinfo=dt_util.DEFAULT_TIME_ZONE)
            when = int(when.timestamp())
        return await controller().request_alarm(when, call.data["request_id"], call.data.get("context"))
    async def cancel_alarm(call):
        return await controller().request_alarm(0, call.data["request_id"], call.data.get("context"), cancel=True)
    async def refresh(call):
        await controller().refresh(call.data.get("frequent", False))
    schema = {vol.Required("request_id"): vol.All(cv.string, vol.Length(min=1, max=128)),
              vol.Optional("context"): dict}
    hass.services.async_register(DOMAIN, "activate", activate)
    hass.services.async_register(DOMAIN, "set_alarm", set_alarm,
        schema=vol.Schema({**schema, vol.Required("when"): vol.Any(cv.datetime, vol.Coerce(int))}), supports_response=SupportsResponse.OPTIONAL)
    hass.services.async_register(DOMAIN, "cancel_alarm", cancel_alarm,
        schema=vol.Schema(schema), supports_response=SupportsResponse.OPTIONAL)
    hass.services.async_register(DOMAIN, "refresh", refresh, schema=vol.Schema({vol.Optional("frequent", default=False): cv.boolean}))
    return True

async def async_setup_entry(hass, entry):
    controller = Controller(hass, entry)
    hass.data.setdefault(DOMAIN, {})[entry.entry_id] = controller
    await controller.start()
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    return True

async def async_unload_entry(hass, entry):
    if not await hass.config_entries.async_unload_platforms(entry, PLATFORMS):
        return False
    await hass.data[DOMAIN].pop(entry.entry_id).stop()
    return True
