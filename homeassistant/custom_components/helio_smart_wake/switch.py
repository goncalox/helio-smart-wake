from homeassistant.components.switch import SwitchEntity
from .entity import HelioEntity

async def async_setup_entry(hass, entry, async_add_entities):
    c = hass.data["helio_smart_wake"][entry.entry_id]
    async_add_entities(HelioSwitch(c, key, name) for key, name in
        (("enabled", "Smart wake"), ("learning", "Model learning"), ("automatic", "Automatic model updates")))

class HelioSwitch(HelioEntity, SwitchEntity):
    @property
    def is_on(self):
        return self.controller.settings[self.key]

    async def async_turn_on(self, **kwargs):
        await self.controller.setting(self.key, True)

    async def async_turn_off(self, **kwargs):
        await self.controller.setting(self.key, False)
