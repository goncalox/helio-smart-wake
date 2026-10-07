from datetime import datetime, timezone
from homeassistant.components.sensor import SensorEntity, SensorDeviceClass
from homeassistant.const import EntityCategory
from .entity import HelioEntity

FIELDS = [("status", "Status"), ("mode", "Owner"), ("onset", "Sleep onset"),
          ("target", "Full sleep target"), ("confirmed", "Verified alarm"), ("awake", "Awake compensation"),
          ("model_stage", "Model stage"), ("score", "Experimental sleep score"),
          ("score_coverage", "Score coverage"), ("candidate", "Candidate model"), ("checked", "Validation nights")]

async def async_setup_entry(hass, entry, async_add_entities):
    controller = hass.data["helio_smart_wake"][entry.entry_id]
    async_add_entities(HelioSensor(controller, key, name) for key, name in FIELDS)

class HelioSensor(HelioEntity, SensorEntity):
    def __init__(self, controller, key, name):
        super().__init__(controller, key, name)
        if key in ("onset", "target", "confirmed"):
            self._attr_device_class = SensorDeviceClass.TIMESTAMP
        if key in ("candidate", "checked", "score_coverage"):
            self._attr_entity_category = EntityCategory.DIAGNOSTIC
        if key == "awake":
            self._attr_native_unit_of_measurement = "min"
        if key in ("score", "score_coverage"):
            self._attr_suggested_display_precision = 0

    @property
    def native_value(self):
        value = self.controller.state.get(self.key)
        if self.key in ("onset", "target", "confirmed"):
            return datetime.fromtimestamp(value, timezone.utc) if value else None
        if self.key == "model_stage":
            return {4: "Light", 5: "Deep", 7: "Awake", 8: "REM"}.get(value, "Unavailable") if self.controller.state.get("model_valid") else "Unavailable"
        if self.key == "score" and not self.controller.state.get("score_nights"):
            return None
        return value

    @property
    def extra_state_attributes(self):
        if self.key == "status":
            return {key: self.controller.state.get(key) for key in ("connected", "cursor", "transport", "model_sample", "champion", "candidate", "checked", "score_nights")}
        if self.key == "score":
            return {"experimental": True, "coverage": self.controller.state.get("score_coverage"), "shown_on_esp_screen": False}
        return None
