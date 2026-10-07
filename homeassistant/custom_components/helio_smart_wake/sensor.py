from datetime import datetime, timezone
from homeassistant.components.sensor import SensorEntity, SensorDeviceClass
from homeassistant.const import EntityCategory
from .entity import HelioEntity

FIELDS = [("observations", "Sleep observations"), ("command_status", "Alarm instructions"), ("status", "Status"), ("mode", "Owner"), ("onset", "Sleep onset"),
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
        if self.key == "observations":
            return max(self.controller.state.get("read_at", 0), self.controller.state.get("wear_read_at", 0))
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
        if self.key == "observations":
            return {key: self.controller.state.get(key, 0) for key in (
                "night", "session_end", "onset", "awake", "target", "raw_onset", "raw_awake", "band_valid", "complete", "nap", "band_stage",
                "through", "read_at", "model_valid", "model_stage", "model_sample", "model_read_at", "wear_state", "wear_sample", "wear_read_at")}
        if self.key == "command_status":
            return {"pending": bool(self.controller.saved.get("command")), "last_instruction": self.controller.saved.get("last_instruction", {}),
                    "verified_epoch": self.controller.saved.get("verified_epoch", 0), "manual": self.controller.saved.get("manual", 0)}
        if self.key == "status":
            return {key: self.controller.state.get(key) for key in ("connected", "cursor", "transport", "model_sample", "champion", "candidate", "checked", "score_nights")}
        if self.key == "score":
            return {"experimental": True, "coverage": self.controller.state.get("score_coverage"), "shown_on_esp_screen": False}
        return None
