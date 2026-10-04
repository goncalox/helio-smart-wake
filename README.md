# Helio Smart Wake

ESPHome firmware for a Waveshare ESP32-S3-GEEK and Amazfit Helio Strap.

The ESP32 authenticates over Bluetooth, reads sleep/activity summaries, saves
alarms with readback verification, and runs an experimental sleep-stage model.
Home Assistant provides controls and status; runtime timing, inference and
alarm scheduling run on the ESP32 without an always-on computer or Home Assistant.
Once successfully saved, an alarm runs on the strap itself.

## Wake behavior

- Wait for stable night sleep onset: at least 90 minutes of recorded night data
  and two matching observations at least five minutes apart.
- Target = onset + desired sleep duration + accepted recorded awake minutes.
- There is no fixed clock-time deadline and no automatically created clock-time
  fallback before onset is established.
- During the early window, choose an earlier alarm only when both the strap and
  the experimental model report fresh, aligned **Light or Awake**. Mixed pairs
  qualify too; no repeated Awake readings are required.
- Schedule that alarm at least two minutes ahead, round to a minute, verify it
  by reading it back, and keep it fixed for that night.
- If either source reports Deep or REM, is invalid, missing or stale, keep the full-target alarm.
- Preserve other Zepp alarms; manual bridge controls take precedence for the night.
- Settings, alarm ownership and dated night state survive restarts.

Factory defaults are **8.5 hours**, a **15-minute early window**, smart wake
**off**, and sleep monitoring **on**.
The existing installation uses a saved **60-minute window** with smart wake on;
saved settings override defaults and are not stored in this repository.
The early-window control accepts **0–60 minutes** in five-minute steps.
The configured timezone is `Europe/Lisbon`; edit `helio-test.yaml` for another zone.

## Setup

Tested with ESPHome **2026.8.0**, the ESP-IDF framework, and the Waveshare
ESP32-S3-GEEK with its 2 MB quad PSRAM and onboard 240×135 display.

1. Clone this repository into an ESPHome configuration directory.
2. Copy `secrets.example.yaml` to `secrets.yaml` and replace every placeholder.
3. Supply the strap's Bluetooth address and its 32-hex-character Zepp authentication
   key; do not commit your local secrets.
4. Generate an ESPHome API encryption key locally, for example:
   `python3 -c "import os,base64; print(base64.b64encode(os.urandom(32)).decode())"`.
5. Install the dependencies in a virtual environment: `pip install -r requirements.txt`.
6. Validate with `esphome config helio-bridge.yaml`, compile with
   `esphome compile helio-bridge.yaml`, and perform the first USB install with
   `esphome run helio-bridge.yaml` or ESPHome Device Builder.
7. Add the device to Home Assistant's ESPHome integration using the local API key.
8. Enable **Helio Smart Wake**, choose the target and window, and leave
   **Helio Sleep Monitoring** enabled.

For an existing ESPHome device, keep its existing base configuration and secrets,
copy `components/`, `helio-test.yaml` and any desired desktop tools, then include
the package as shown in `helio-bridge.yaml`.
Do not replace an existing API key or partition table just to use this source.

Wi-Fi and internet access provide SNTP time after a cold boot; the clock does
not come from Home Assistant.
The API disconnection reboot timer is disabled.
The strap must be within Bluetooth range when reading or changing its alarm.
Alarm times use the strap's existing local clock; this component does not set it.

## Controls and display

Home Assistant exposes battery, connection, sleep freshness, clock sync,
recorded awake time, model stage, smart target, saved alarm and operation status.
Manual alarm controls offer Once, Every day, Weekdays and Weekends.
Changing the time selector alone does not write an alarm; press **Set Helio Alarm**
and wait for **Saved and verified**.
**Cancel Helio Alarm** cancels only the bridge-owned alarm after checking ownership.

The dimmed, always-visible display shows strap battery, reading age and recent
contact status, at 10% brightness by default.
The connection indicator accounts for normal idle Bluetooth disconnections.

Settings are captured when a night is armed.
To rearm with changed settings, disable smart wake, wait for verified cancellation,
then enable it again.
An independent backup alarm can be set directly in Zepp.

## Logging and desktop tools

The ESP32 records raw sleep records, minute activity summaries, transfer errors,
clock syncs, alarm/session changes, model features/scores and early-wake decisions
in a lossless rolling flash journal.
Its budget is 256 KiB, with up to 384 batches and a free-entry reserve for settings;
the tested ESPHome layout has a 448 KiB NVS partition.
Retention depends on volume, not a guaranteed number of nights.
Committed batches survive restarts; unflushed events can be lost on power failure.
Logging does not require a Mac or Home Assistant.

Read-only inspection:

```sh
python tools/helio_api.py inspect --config helio-bridge.yaml --host helio-bridge.local
python diagnostics/download.py --config helio-bridge.yaml --host helio-bridge.local --output logs/review
```

The downloader checks batch identity, byte counts and CRC, reports missing
batches, and decodes model and early-gate observations.
Downloaded records contain personal sleep/activity information and are ignored by Git.
Use the locally installed ESPHome device configuration when its API key is stored there.

`tools/helio_api.py` also offers explicit `test`, `sleep`, `set`, `cancel`,
`smart-on` and `smart-off` operations; these contact the strap or change alarms/settings.
`diagnostics/prepare_shadow.py` prepares causal five-minute HR/movement features.
`diagnostics/test_stage_model.py` runs a retrospective model evaluation on supplied
local records; it does not deploy coefficients or change alarms.
`diagnostics/export_stage_model.py --output PATH` exports the fixed model header.

## Experimental model limits

The fixed model `20261003-v1` uses latest heart rate, five-minute mean and standard
deviation of heart rate, mean movement intensity and summed steps.
It excludes stage-bearing activity bytes from its input.
The inputs are device-processed minute summaries, not raw optical or motion waveforms.

Predictions require five contiguous minute rows, at least three valid heart rates
and a valid latest heart rate while worn.
For early waking, each source must report Light or Awake, and both samples must be no more than three minutes old, both reads
no more than 90 seconds old, and the sample intervals aligned within one minute.

The model was developed using one night and later strap labels, with only within-night
block evaluation; it has no independent-night or clinical validation.
Its scores are uncalibrated and the newest strap stage can be revised later.
The deployed fit is the full-night fit, not a held-out fold.
Agreement at scheduling cannot establish the physiological sleep stage when
the strap vibrates two to three minutes later.
The full sleep-duration alarm remains the fallback when the early gate is not met.

## Tests

Run `python tests/run.py` with Python 3.10+, a C/C++ compiler, OpenSSL with
`sect163r2` support, and the desktop dependencies.
The tests run locally without contacting or flashing any device.
They cover alarm ownership, protocol framing, sleep/awake accounting, data transfer
validation, dated sessions and legacy migration, DST, the Light-or-Awake gate, causal
features, fixed Python/C++ inference parity and rolling log integrity.
Synthetic fixtures are included; private overnight recordings are not.

## Source layout

- `components/helio_bridge/`: the complete deployed custom ESPHome component.
- `helio-test.yaml`: display, controls, polling, SNTP and local component package.
- `helio-bridge.yaml`: portable base configuration with secret references.
- `diagnostics/`: decoder, journal downloader, feature/model tools and frozen coefficients.
- `tools/`: portable encrypted ESPHome API helper.
- `tests/`: host regression tests and synthetic inference checks.

## References and attribution

- [Atlas](https://github.com/atlas-healthapp/atlas), protocol reference commit
  `8a0b4d1a5e6a898ce20640a686403c1b6622acd8`.
- [Gadgetbridge](https://codeberg.org/Freeyourgadget/Gadgetbridge), Zepp alarm service reference.
- [tiny-ECDH-c](https://github.com/kokke/tiny-ECDH-c), public-domain B-163 implementation;
  its original license is preserved in `components/helio_bridge/LICENSE-tiny-ECDH-c`.
- [Waveshare board documentation](https://docs.waveshare.com/ESP32-S3-GEEK).
- [ESPHome](https://esphome.io/).

Private credentials, personal overnight records and generated firmware are not included.
