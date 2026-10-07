from homeassistant.components.binary_sensor import BinarySensorEntity, BinarySensorDeviceClass
from .entity import HelioEntity

async def async_setup_entry(hass, entry, async_add_entities):
    controller = hass.data["helio_smart_wake"][entry.entry_id]
    async_add_entities([BridgeConnected(controller)])

class BridgeConnected(HelioEntity, BinarySensorEntity):
    _attr_device_class = BinarySensorDeviceClass.CONNECTIVITY
    def __init__(self, controller):
        super().__init__(controller, "bridge_connected", "Bridge connected")
    @property
    def is_on(self):
        return self.controller.connected
