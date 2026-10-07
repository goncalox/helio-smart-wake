"""Bind to an existing encrypted ESPHome entry; never request or copy its secret."""
import voluptuous as vol
from homeassistant import config_entries
from homeassistant.helpers import selector

DOMAIN = "helio_smart_wake"

class ConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    VERSION = 1

    async def async_step_user(self, user_input=None):
        if user_input:
            entry_id = user_input["esphome_entry"]
            await self.async_set_unique_id(entry_id)
            self._abort_if_unique_id_configured()
            return self.async_create_entry(title="Helio Smart Wake", data=user_input)
        choices = [{"value": e.entry_id, "label": e.title}
                   for e in self.hass.config_entries.async_entries("esphome")]
        return self.async_show_form(step_id="user", data_schema=vol.Schema({
            vol.Required("esphome_entry"): selector.SelectSelector(
                selector.SelectSelectorConfig(options=choices, mode="dropdown"))}))
