"""Durable encrypted transport, single ownership, causal inputs and verified commands."""
import asyncio
from datetime import datetime, timezone
import json
import logging
from pathlib import Path
import re
import struct
import time
import zlib
from aioesphomeapi import APIClient
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.storage import Store
from homeassistant.helpers.dispatcher import async_dispatcher_send
from .journal import decode, events

DOMAIN = "helio_smart_wake"
_LOGGER = logging.getLogger(__name__)
DEFAULTS = {"hours": 8.5, "window": 30, "enabled": True, "learning": True, "automatic": True}

class Controller:
    def __init__(self, hass, entry):
        self.hass, self.entry = hass, entry
        self.store = Store(hass, 1, DOMAIN + "." + entry.entry_id)
        self.saved = {}
        self.state = {"status": "Starting", "mode": "Shadow"}
        self.settings = DEFAULTS.copy()
        self.client = None
        self.process = None
        self.task = None
        self.control_task = None
        self.transfer_lock = asyncio.Lock()
        self.lock = asyncio.Lock()
        self.engine_lock = asyncio.Lock()
        self.queue = asyncio.Queue(maxsize=4096)
        self.values, self.entities, self.services = {}, {}, {}
        self.connected = False
        self.transport = {}
        self.signal = DOMAIN + "_" + entry.entry_id
        self.raw_dir = Path(hass.config.path("helio_history")) / entry.entry_id
        self.last_read_request = 0
        self.last_saved = None

    async def start(self):
        self.saved = await self.store.async_load() or {}
        self.settings.update(self.saved.get("settings", {}))
        engine = Path(__file__).with_name("helio-engine")
        self.process = await asyncio.create_subprocess_exec(str(engine), stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE, limit=500000)
        if self.saved.get("checkpoint"):
            await self.engine("RESTORE " + self.saved["checkpoint"])
        self.task = self.entry.async_create_background_task(self.hass, self.run(), "Helio journal")
        self.control_task = self.entry.async_create_background_task(self.hass, self.control(), "Helio live decisions")

    async def restart_worker(self):
        # Recover only from the last committed HA Store checkpoint.
        committed = await self.store.async_load()
        engine = Path(__file__).with_name("helio-engine")
        self.process = await asyncio.create_subprocess_exec(str(engine), stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE, limit=500000)
        if committed:
            self.saved = committed
            self.settings.update(committed.get("settings", {}))
            self.last_saved = None
            if committed.get("checkpoint"):
                await self.engine("RESTORE " + committed["checkpoint"])
        await self.audit("model_process_recovered")

    async def stop(self):
        tasks = [t for t in (self.task, self.control_task) if t]
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        if self.client:
            await self.client.disconnect()
        if self.process:
            self.process.terminate()
            await self.process.wait()

    async def engine(self, command):
        async with self.engine_lock:
            self.process.stdin.write((command + "\n").encode())
            await self.process.stdin.drain()
            line = await asyncio.wait_for(self.process.stdout.readline(), 20)
            if not line:
                raise HomeAssistantError("Helio model process stopped")
            answer = json.loads(line)
            if not answer.get("ok"):
                raise HomeAssistantError(answer.get("error", "Engine rejected input"))
            self.state.update({k: v for k, v in answer.items() if k != "checkpoint"})
            return answer

    async def persist(self):
        answer = await self.engine("SAVE")
        self.saved["checkpoint"] = answer["checkpoint"]
        self.saved["settings"] = self.settings.copy()
        versions = {k: self.state.get(k, 0) for k in ("champion", "candidate", "checked")}
        if self.saved.get("model_versions") != versions:
            stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%f")
            await self.archive(f"models-{stamp}.json", json.dumps({"versions": versions,
                "settings": self.settings, "checkpoint": answer["checkpoint"]}).encode())
            await self.audit("model_checkpoint", **versions)
            self.saved["model_versions"] = versions
        serialized = json.dumps(self.saved, sort_keys=True)
        if serialized != self.last_saved:
            await self.store.async_save(self.saved)
            self.last_saved = serialized

    def publish(self):
        self.state["mode"] = "Home Assistant" if self.saved.get("active") and self.transport.get("owner") == 1 else "ESP32" if self.transport.get("owner") == 0 else "Paused"
        self.state["phase"] = "Active" if self.saved.get("active") else "Shadow"
        self.state["connected"] = self.connected
        self.state["cursor"] = self.saved.get("cursor", 0)
        self.state["transport"] = self.transport.copy()
        async_dispatcher_send(self.hass, self.signal)

    async def setting(self, key, value):
        async with self.lock:
            self.settings[key] = value
            await self.persist()
            self.publish()

    def on_state(self, state):
        entity = self.entities.get(state.key)
        if not entity:
            return
        self.values[entity.name] = getattr(state, "state", None)
        if entity.name == "Helio Controller Transport":
            try:
                self.transport = json.loads(state.state)
            except (ValueError, AttributeError):
                self.transport = {}

    def on_log(self, message):
        clean = re.sub(rb"\x1b\[[0-9;]*[A-Za-z]", b"", message.message)
        match = re.search(rb"HLG2 (.*)", clean)
        if match:
            try:
                self.queue.put_nowait(match[1].decode())
            except asyncio.QueueFull:
                self.connected = False  # no partial transfer can pass CRC

    async def connect(self):
        source = self.hass.config_entries.async_get_entry(self.entry.data["esphome_entry"])
        if not source:
            raise HomeAssistantError("ESPHome entry removed")
        config = source.data
        self.client = APIClient(config["host"], config.get("port", 6053), password=config.get("password", ""),
            noise_psk=config.get("noise_psk"), client_info="Helio HA Controller")
        await self.client.connect(login=True)
        entities, services = await self.client.list_entities_services()
        self.entities = {e.key: e for e in entities}
        self.services = {s.name: s for s in services}
        required = {"helio_download_diagnostics", "helio_controller_alarm", "helio_controller_fast"}
        if not required <= self.services.keys():
            raise HomeAssistantError("Bridge transport firmware is missing")
        self.client.subscribe_states(self.on_state)
        self.client.subscribe_logs(self.on_log, log_level=3, dump_config=False)
        await asyncio.sleep(2)
        self.connected = True

    async def action(self, name, data):
        await self.client.execute_service(self.services[name], data)

    async def transfer(self, sequence):
        async with self.transfer_lock:
            return await self._transfer(sequence)

    async def _transfer(self, sequence):
        while not self.queue.empty():
            self.queue.get_nowait()
        await self.action("helio_download_diagnostics", {"sequence": sequence})
        expected, raw, seq = None, bytearray(), None
        deadline = time.monotonic() + 100
        while time.monotonic() < deadline:
            line = await asyncio.wait_for(self.queue.get(), 12)
            fields = dict(re.findall(r"(\w+)=([^ ]+)", line))
            if line.startswith(("ERROR", "MISSING")):
                raise HomeAssistantError("Bridge journal transfer failed")
            if line.startswith("INDEX"):
                return {k: int(v) for k, v in fields.items()}
            if line.startswith("BEGIN"):
                seq, expected = int(fields["seq"]), int(fields["bytes"])
                if not 16 <= expected <= 400016:
                    raise ValueError("Transfer too large")
                if sequence == -2 and seq != 0 or sequence >= 0 and seq != sequence:
                    raise ValueError("Wrong transfer sequence")
            elif line.startswith("DATA"):
                if expected is None or int(fields["seq"]) != seq or int(fields["offset"]) != len(raw):
                    raise ValueError("Missing transfer chunk")
                raw.extend(bytes.fromhex(fields["hex"]))
                if len(raw) > expected:
                    raise ValueError("Oversized transfer")
            elif line.startswith("END"):
                if int(fields["seq"]) != seq or len(raw) != expected:
                    raise ValueError("Incomplete transfer")
                return seq, bytes(raw), decode(raw, seq)
        raise TimeoutError("Transfer timed out")

    async def import_bridge(self):
        index = await self.transfer(-1)
        _, blob, (magic, raw) = await self.transfer(-2)
        if magic != b"HLS1":
            raise ValueError("Expected model snapshot")
        answer = await self.engine("IMPORT " + raw.hex())
        self.saved.update(cursor=index["first"] - 1 if not self.saved.get("checkpoint") else index["next"] - 1, imported_at=answer["now"], import_stamp=answer["import_stamp"],
                          seen=[], command=None, manual=self.transport.get("manual", 0))
        await self.archive("migration-snapshot.bin", blob)
        await self.persist()

    async def archive(self, name, data):
        def write():
            self.raw_dir.mkdir(parents=True, exist_ok=True)
            path = self.raw_dir / name
            temp = path.with_suffix(path.suffix + ".tmp")
            temp.write_bytes(data)
            temp.replace(path)
        await self.hass.async_add_executor_job(write)

    async def audit(self, event, **data):
        row = json.dumps({"utc": datetime.now(timezone.utc).isoformat(), "event": event, **data}) + "\n"
        def write():
            self.raw_dir.mkdir(parents=True, exist_ok=True)
            with (self.raw_dir / (datetime.now(timezone.utc).strftime("%Y%m%d") + "-audit.jsonl")).open("a") as out:
                out.write(row)
        await self.hass.async_add_executor_job(write)

    async def ingest(self, raw):
        seen = list(self.saved.get("seen", []))
        for kind, stamp, uptime, payload in events(raw):
            identity = f"{stamp}:{uptime}:{kind}:{zlib.crc32(payload)}"
            if identity in seen or stamp < self.saved.get("imported_at", 0):
                continue
            if not 1577836800 <= stamp <= time.time() + 5:
                raise ValueError("Device clock invalid")
            if kind == 1:
                await self.engine(f"SLEEP {stamp} {payload.hex()}")
            elif kind == 9:
                if len(payload) < 17 or payload[0] != 1 or (len(payload) - 17) % 8:
                    raise ValueError("Invalid activity event")
                start = struct.unpack_from("<I", payload, 5)[0]
                await self.engine(f"ACTIVITY {stamp} {start} {payload[17:].hex() or "-"}")
            elif kind in (11, 17) and not self.saved.get("active"):
                if len(payload) < 44:
                    raise ValueError("Invalid model audit")
                sample = struct.unpack_from("<I", payload, 4)[0]
                if payload[1] and self.state.get("model_sample") == sample:
                    match = self.state.get("model_valid") and self.state.get("model_stage") == payload[2]
                    counter = "shadow_pass" if match else "shadow_fail"
                    self.saved[counter] = self.saved.get(counter, 0) + 1
                    await self.audit("shadow_model", sample=sample, matches=bool(match))
            seen.append(identity)
            seen = seen[-2048:]
        self.saved["seen"] = seen

    async def reconcile(self, now):
        if not self.saved.get("active") or self.transport.get("owner") != 1:
            return
        manual = self.transport.get("manual")
        if manual is not None and manual != self.saved.get("manual"):
            self.saved["manual"] = manual
            self.saved["command"] = None
            await self.engine(f"MANUAL {now}")
            await self.persist()
            await self.audit("manual_override")
        command = self.saved.get("command")
        if command:
            if self.transport.get("id") == command["command_id"] and self.transport.get("status") == 2:
                if self.transport.get("epoch") != command["epoch"]:
                    raise HomeAssistantError("Wrong verified alarm epoch")
                await self.engine(f"ACK {now} 1")
                self.saved["command"] = None
                await self.persist()
                await self.audit("alarm_verified", **command)
            elif now > command["expires"]:
                await self.engine(f"ACK {now} 0")
                self.saved["command"] = None
                await self.persist()
                await self.audit("alarm_unverified", **command)
            elif self.transport.get("idle") and now >= command.get("retry_at", 0):
                await self.persist()  # a prior failed save must not permit an in-memory retry
                await self.action("helio_controller_alarm", {k: command[k] for k in ("command_id", "epoch", "expires", "previous", "cancel", "follow")})
                command["retry_at"] = now + 10
            return
        if self.state.get("pending"):
            command = {"command_id": self.saved.get("next_id", 0) + 1,
                "epoch": self.state["epoch"], "previous": self.state["previous"],
                "expires": now + 100, "cancel": bool(self.state["cancel"]), "follow": bool(self.state["follow"])}
            self.saved["next_id"] = command["command_id"]
            self.saved["command"] = command
            await self.persist()  # persist intent and uncertain policy state BEFORE network I/O
            await self.audit("alarm_requested", **command)
            if self.transport.get("idle"):
                await self.action("helio_controller_alarm", {k: command[k] for k in ("command_id", "epoch", "expires", "previous", "cancel", "follow")})
                command["retry_at"] = now + 10

    async def tick(self):
        now = int(time.time())
        s = self.settings
        if self.saved.get("active") and self.transport.get("owner") != 1:
            self.state["status"] = "ESP32 owns decisions; Home Assistant paused"
            return
        if now < self.state.get("now", 0) - 5:
            raise HomeAssistantError("Clock moved backwards")
        await self.engine(f"TICK {now} {s['hours']} {s['window']} {int(s['enabled'])} {int(s['learning'])} {int(s['automatic'])}")
        if self.saved.get("active"):
            if self.transport.get("owner") != 1:
                self.state["status"] = "ESP32 owns decisions; Home Assistant paused"
                return
            await self.reconcile(now)
            if self.state.get("fast"):
                await self.action("helio_controller_fast", {"expires": now + 90})
                if now - self.last_read_request >= 60:
                    await self.action("helio_read_sleep", {})
                    self.last_read_request = now
        await self.persist()

    async def activate(self):
        async with self.lock:
            if not self.connected or not self.transport.get("idle"):
                raise HomeAssistantError("Wait until the bridge is connected and idle")
            if self.saved.get("shadow_fail", 0):
                raise HomeAssistantError("Shadow model disagreement needs investigation")
            if self.saved.get("active"):
                return {"owner": "Home Assistant"}
            index = await self.transfer(-1)
            if self.saved.get("cursor", 0) < index["next"] - 1:
                raise HomeAssistantError("Wait for journal recovery to finish")
            await self.import_bridge()  # adopt the exact last verified alarm and model state
            if max(self.state.get(k, 0) for k in ("confirmed", "attempted", "follow_attempted")) > time.time():
                raise HomeAssistantError("Transfer ownership outside a pending wake alarm")
            self.saved["active"] = True
            self.saved["next_id"] = max(self.saved.get("next_id", 0), self.transport.get("id", 0))
            await self.persist()  # HA activation durable before disabling local decisions
            select = next(e for e in self.entities.values() if e.name == "Helio Alarm Controller")
            self.client.select_command(select.key, "Home Assistant")
            for _ in range(15):
                await asyncio.sleep(1)
                if self.transport.get("owner") == 1:
                    await self.audit("owner_activated")
                    self.publish()
                    return {"owner": "Home Assistant", "history_transferred": True}
            self.saved["active"] = False
            await self.persist()
            raise HomeAssistantError("ESP32 did not confirm the ownership change")

    async def run(self):
        while True:
            try:
                if not self.connected:
                    await self.connect()
                    async with self.lock:
                        if not self.saved.get("checkpoint"):
                            for key, name in (("hours", "Helio Sleep Target"), ("window", "Helio Light Sleep Window")):
                                value = self.values.get(name)
                                if value is not None:
                                    self.settings[key] = float(value) if key == "hours" else int(value)
                            for key, name in (("enabled", "Helio Smart Wake"), ("learning", "Helio Model Learning"), ("automatic", "Helio Automatic Model Updates")):
                                if name in self.values:
                                    self.settings[key] = bool(self.values[name])
                            await self.import_bridge()
                index = await self.transfer(-1)
                if index["pending"]:
                    seq, blob, (magic, raw) = await self.transfer(-3)
                    if magic != b"HLP2":
                        raise ValueError("Expected live journal")
                    await self.archive("pending.bin", blob)
                    async with self.lock:
                        await self.ingest(raw)
                        await self.persist()
                        self.publish()
                async with self.lock:
                    cursor = self.saved.get("cursor", index["first"] - 1)
                    if cursor + 1 < index["first"]:
                        await self.audit("journal_gap", first=index["first"], cursor=cursor)
                        self.saved["missing_batches"] = self.saved.get("missing_batches", 0) + index["first"] - cursor - 1
                        cursor = index["first"] - 1
                # Bounded replay never holds the decision lock during network transfers.
                for seq in range(cursor + 1, min(index["next"], cursor + 3)):
                    _, blob, (_, raw) = await self.transfer(seq)
                    await self.archive(f"{seq:08d}.bin", blob)
                    async with self.lock:
                        await self.ingest(raw)
                        self.saved["cursor"] = max(self.saved.get("cursor", 0), seq)
                        await self.persist()
                        self.publish()
                await asyncio.sleep(5)
            except asyncio.CancelledError:
                raise
            except Exception as err:
                self.connected = False
                self.state["status"] = "Controller unavailable; last verified strap alarm retained"
                self.publish()
                _LOGGER.warning("Helio controller reconnect: %s", type(err).__name__)
                await self.audit("controller_error", error=type(err).__name__, detail=str(err)[:300])
                if self.client:
                    await self.client.disconnect()
                if self.process.returncode is not None:
                    try:
                        async with self.lock:
                            await self.restart_worker()
                    except Exception:
                        _LOGGER.error("Helio model recovery failed; alarm decisions stopped")
                await asyncio.sleep(10)

    async def control_once(self):
        async with self.lock:
            if self.connected and self.saved.get("checkpoint"):
                await self.tick()
                self.publish()

    async def control(self):
        while True:
            try:
                await self.control_once()
            except asyncio.CancelledError:
                raise
            except Exception as err:
                self.connected = False
                self.state["status"] = "Controller unavailable; last verified strap alarm retained"
                self.publish()
                _LOGGER.warning("Helio live decisions paused: %s", type(err).__name__)
                await self.audit("control_error", error=type(err).__name__, detail=str(err)[:300])
                if self.client:
                    await self.client.disconnect()
            await asyncio.sleep(2)
