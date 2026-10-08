# Helio Smart Wake

ESPHome firmware for a Waveshare ESP32-S3-GEEK and Amazfit Helio Strap.

The ESP32 authenticates over Bluetooth, reads sleep/activity summaries, saves
alarms with readback verification, and displays battery, Helio score and duration.
This repository contains ESP32 firmware, its configuration, documentation,
diagnostic tools and firmware tests.
Home Assistant integrations and automation configuration are maintained separately.
Home Assistant is the only wake decision owner; the firmware contains no local
wake scheduler, full-target fallback or autonomous reminder loop.
The ESP32 reads the strap and executes explicit alarm commands with ownership,
expiry, persistence and readback checks.
No always-on Mac is needed.
Once successfully saved, an alarm runs on the strap itself.

## Personal sleep-quality score

A separate experimental **0–100 personal sleep score** runs on the selected controller using
nighttime duration, awakening length/clusters, timing, heart-rate patterns,
movement bursts and recent short nights.
It needs no daily ratings or daytime readings and does not learn toward Helio's score.
Personal timing/physiological baselines adapt after seven eligible prior nights,
with short-night context available after three; missing components and
provisional records are visible in Home Assistant and the onboard comparison audit.
This is a heuristic pilot, not a validated predictor or proven improvement over Helio.
The v2 upgrade preserves v1 scores and recovers available recent minute data from
the ESP32's existing logs; HA migration preserves the complete saved score history.
See [formula, adaptation and comparison limits](docs/personal-sleep-score.md).

## Wake ownership

All sleep targets, wake windows, Awake/Light decisions and reminders belong to
Home Assistant's automations and integration, maintained outside this repository.
The ESP32 cannot select an alarm from sleep, stage, wear or clock changes.
`Helio Alarm Controller` has only the **Home Assistant** option; saved legacy ESP32
ownership is migrated on boot while command IDs, manual generation and the last
verified alarm remain intact.
There is no ESP32 takeover during a Home Assistant outage.
A previously saved strap alarm can still ring locally; new decisions need HA.
Other Zepp alarms are preserved, and explicit manual bridge controls remain.

The ESP polls sleep normally every five minutes and activity at most every four
minutes; HA can request a bounded minute-read lease near its chosen wake threshold.
The configured timezone is `Europe/Lisbon`; edit `helio-test.yaml` for another zone.

## Setup

Tested with ESPHome **2026.8.0**, the ESP-IDF framework, and the Waveshare
ESP32-S3-GEEK with its 2 MB quad PSRAM and onboard 135×240 display.

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
8. Configure the separately maintained HA integration and wake automation, and
   leave **Helio Sleep Monitoring** enabled; the ESP32 cannot decide wake times.

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
recorded awake time, strap sleep score, saved alarm and operation status.
Manual alarm controls offer Once, Every day, Weekdays and Weekends.
Changing the time selector alone does not write an alarm; press **Set Helio Alarm**
and wait for **Saved and verified**.
**Cancel Helio Alarm** cancels only the bridge-owned alarm after checking ownership.

The dimmed, always-visible portrait display uses a full-width read-health banner
and three vertically stacked values separated by thin lines: **BATTERY** (%),
**SLEEP SCORE** (0–100) and **TOTAL SLEEP** (hours:minutes asleep, excluding awake time).
All labels are uppercase, with a bold font, consistent spacing and separate smaller percent sign.
The layout uses the complete 135×240 screen at rotation 180° and 10% brightness.
There are no signal readings or extra headings.
Unknown values show dashes; values whose last receipt is over 15 minutes old fade grey.
The nightly duration is selected separately from the most recent night-or-nap stage,
so a newer nap cannot replace it; it uses the strap's nightly summary, falling back
to complete night-stage accounting for an ongoing night.
It is also exposed as `Helio Night Sleep Duration` in Home Assistant (minutes).
The health banner shows **READ OK** (green) only after successful validated sleep and
activity transfers, **READING** (blue) while fetching, **READ FAIL** (red) after a failed
transfer, **WAITING** (amber) before first success or when reads become overdue,
and **READ OFF** (grey) when sleep monitoring is disabled.
The same status is exposed as `Helio Read Status` in Home Assistant.
Freshness uses transfer receipt time, not the age of the last sleep stage;
a strap that returns no new records can still have healthy communication.
The freshness allowance is the polling interval plus the 90-second read timeout:
6.5 minutes normally, 2.5 minutes during an HA minute-read lease.
Normal idle Bluetooth disconnections and activity reads preempted for an alarm
are not counted as failed transfers; authenticated contact alone cannot clear a read failure.
The sleep score is read from byte `0x16` of the validated sleep record, matching
Gadgetbridge's Huami decoder; it is not computed by our model and does not affect alarms.
It can change during the night or in later strap revisions.
The latest valid night is selected independently of newer naps and record arrival order.
The sleep score is also exposed to Home Assistant and decoded in downloaded logs.

The next successful fetch can recover stored sleep records within the last 48 hours
and activity records within the last 30 minutes, if still available on the strap.
It cannot recover a missed real-time opportunity to schedule an early alarm.

Sleep target and wake-window controls live in Home Assistant.
The firmware no longer exposes a local smart-wake switch or target/window controls.
An independent backup alarm can be set directly in Zepp.

## Logging and desktop tools

The ESP32 records raw sleep records, minute activity summaries, transfer errors,
clock syncs, explicit alarm commands and transfer observations
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

`tools/helio_api.py` also offers explicit `test`, `sleep`, `set` and `cancel`
operations; these contact the strap or change alarms/settings.
Legacy `smart-on` and `smart-off` are rejected because the ESP switch is removed.
`diagnostics/prepare_shadow.py` prepares causal five-minute HR/movement features.
`diagnostics/test_stage_model.py` runs a retrospective model evaluation on supplied
local records; it does not deploy coefficients or change alarms.
`diagnostics/export_stage_model.py --output PATH` exports the fixed model header.

## Experimental model limits

The starting model `20261003-v1` uses latest heart rate, five-minute mean and standard
deviation of heart rate, mean movement intensity and summed steps.
It excludes stage-bearing activity bytes from its input.
The inputs are device-processed minute summaries, not raw optical or motion waveforms.

Predictions require five contiguous minute rows, at least three valid heart rates
and a valid latest heart rate while worn.
Home Assistant applies the freshness, alignment and stage conditions for its chosen
automation; the firmware does not implement a wake-stage gate.

The model was developed using one night and later strap labels, with only within-night
block evaluation; it has no independent-night or clinical validation.
Its scores are uncalibrated and the newest strap stage can be revised later.
The starting fit is the full-night fit, not a held-out fold.
Accepted adaptive versions keep these same causal features and the original scaling;
normalized adaptive inputs are clipped to ±8, and their learned coefficients persist locally.
Agreement at scheduling cannot establish the physiological sleep stage when
the strap vibrates 30–89 seconds later (plus any scheduling/connection delay).
The firmware does not act on model predictions; HA determines its own wake policy.

## Continuous learning

Learning and automatic checked updates default to **on** in this package.
The Home Assistant integration owns training, comparison, activation
and durable model history; use its **Model learning** and **Automatic model updates** controls.
The descriptions of ESP storage and idle training below document the retained
model data format used for importing existing history; HA owns current training.
The retained model formats use the same feature definitions, settled-label rules
and validation gates; current learning needs no Mac or scheduled desktop task.
Turning learning off stops collection/training and retains the current active model;
turning automatic updates off keeps a qualified candidate waiting until updates are enabled again.

The ESP32 stores up to three dated nights, at most 192 causal observations per night,
using five HR/movement features without stage-bearing input bytes.
Duplicate samples retain their first available features and predictions.
Older rows are trimmed when a night exceeds that bound.
Later complete night labels become eligible only when freshly observed at least
12 hours after the recorded night ends and unchanged for at least an hour.
Naps, gaps, invalid rows and provisional current-night stages cannot train a candidate.
These are settled strap labels, still subject to later revisions, not measured sleep-study truth.

A regularized, class-weighted logistic candidate trains from recent eligible nights in
small slices while Bluetooth is idle and the latest sleep record is no longer live.
Training pauses for Bluetooth/alarms and never changes the active coefficients.
The candidate then stays **frozen for three later nights**, using saved predictions
from those nights before their labels arrive; its training night is excluded.
Each test night needs at least 80 matched observations, with at least 240 overall;
insufficient or missed nights delay activation.
The pooled tests must include at least 40 Light/Awake and 40 Deep/REM labels.
Activation requires overall agreement not to decrease, balanced stage agreement and
Light/Awake precision each to improve by at least three percentage points,
no increase in the Deep/REM-to-Light/Awake error rate, at least 20 wake-ready predictions,
and at least 80% of the old model's Light/Awake recall.
This checks agreement with the strap; it cannot guarantee physiological accuracy
or better results on every future night.

Failed candidates are discarded and another candidate is trained from newer settled nights.
The active version changes only after the whole learning state is successfully saved.
Current dated data, frozen candidates, test counts and active coefficients survive restarts
in one versioned, CRC-checked NVS blob; corrupt data falls back to the starting model.
Observations flush hourly, so a power cut can lose the latest uncommitted hour.
There is no replay of missing observations or automatic import of old desktop downloads.
The feature starts by collecting new nights; its first eligible update needs several nights.

The learner needs about 45 KiB of working memory, prefers PSRAM, and saves under
24 KiB in the existing NVS partition without repartitioning the device.
Learning disables itself if memory/storage is unavailable; the original model continues.
Rolling diagnostic batches may be evicted earlier to leave room for atomic learning saves.
Model versions, candidate votes, evaluation counts and coefficient changes are logged,
so reviews can distinguish baseline predictions from adaptive ones.
Private learned weights and training records remain on the selected controller host,
not in GitHub.
Home Assistant storage and automation configuration are outside this repository.

The model implementation does not choose or write alarms on the ESP32.
Legacy dated session fields remain solely for snapshot compatibility with HA;
they contain no scheduling functions and cannot dispatch an alarm.

## Tests

Run `python tests/run.py` with Python 3.10+, a C/C++ compiler, OpenSSL with
`sect163r2` support, and the desktop dependencies.
The tests run locally without contacting or flashing any device.
They cover alarm ownership, protocol framing, sleep/awake accounting, data transfer
validation, HA-only owner migration and command transport, expiry/duplicate guards,
readback recovery and manual edits, causal features, fixed Python/C++ inference parity
and rolling log integrity.
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
