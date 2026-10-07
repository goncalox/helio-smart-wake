"""Home Assistant owns the wake policy; ESP32 owns BLE transport only."""
from homeassistant.const import Platform
from homeassistant.exceptions import HomeAssistantError
from .controller import Controller

DOMAIN = "helio_smart_wake"
PLATFORMS = [Platform.SENSOR, Platform.NUMBER, Platform.SWITCH]

async def async_setup(hass, config):
    async def activate(call):
        controllers = list(hass.data.get(DOMAIN, {}).values())
        if len(controllers) != 1:
            raise HomeAssistantError("Expected one Helio controller")
        return await controllers[0].activate()
    hass.services.async_register(DOMAIN, "activate", activate)
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
