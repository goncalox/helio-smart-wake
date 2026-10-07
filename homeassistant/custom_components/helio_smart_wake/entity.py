from homeassistant.helpers.entity import Entity
from homeassistant.helpers.dispatcher import async_dispatcher_connect
from homeassistant.helpers.device_registry import DeviceInfo

class HelioEntity(Entity):
    _attr_should_poll = False
    _attr_has_entity_name = True

    def __init__(self, controller, key, name):
        self.controller = controller
        self.key = key
        self._attr_name = name
        self._attr_unique_id = controller.entry.entry_id + "_" + key
        self._attr_device_info = DeviceInfo(identifiers={("helio_smart_wake", controller.entry.entry_id)},
            name="Helio Smart Wake", manufacturer="goncalox", model="Home Assistant controller")

    async def async_added_to_hass(self):
        self.async_on_remove(async_dispatcher_connect(self.hass, self.controller.signal, self.async_write_ha_state))
