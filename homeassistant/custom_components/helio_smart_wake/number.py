from homeassistant.components.number import NumberEntity, NumberMode
from .entity import HelioEntity

async def async_setup_entry(hass, entry, async_add_entities):
    c = hass.data["helio_smart_wake"][entry.entry_id]
    async_add_entities([HelioNumber(c, "hours", "Sleep target", 6, 10, .25, "h"),
                        HelioNumber(c, "window", "Wake window", 0, 60, 5, "min")])

class HelioNumber(HelioEntity, NumberEntity):
    def __init__(self, c, key, name, low, high, step, unit):
        super().__init__(c, key, name)
        self._attr_native_min_value, self._attr_native_max_value = low, high
        self._attr_native_step, self._attr_native_unit_of_measurement = step, unit
        self._attr_mode = NumberMode.BOX

    @property
    def native_value(self):
        return self.controller.settings[self.key]

    async def async_set_native_value(self, value):
        await self.controller.setting(self.key, value if self.key == "hours" else int(value))
