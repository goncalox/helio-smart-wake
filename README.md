# Helio Smart Wake

ESPHome firmware for a Waveshare ESP32-S3-GEEK and Amazfit Helio Strap.

The ESP32 authenticates over Bluetooth, reads sleep/activity summaries, saves
alarms with readback verification, and runs an experimental sleep-stage model.
Home Assistant provides controls and status; runtime timing, inference and
alarm scheduling run on the ESP32 without an always-on computer or Home Assistant.
Once successfully saved, an alarm runs on the strap itself.

## Personal sleep-quality score

A separate experimental **0–100 personal sleep score** runs on the ESP32 using
nighttime duration, awakening length/clusters, timing, heart-rate patterns,
movement bursts and recent short nights.
It needs no daily ratings or daytime readings and does not learn toward Helio's score.
Personal timing/physiological baselines adapt after seven eligible prior nights,
with short-night context available after three; missing components and
provisional records are visible in Home Assistant and the onboard comparison audit.
This is a heuristic pilot, not a validated predictor or proven improvement over Helio.
The v2 upgrade preserves v1 scores and recovers available recent minute data from
the ESP32's existing logs without requiring a computer.
See [formula, adaptation and comparison limits](docs/personal-sleep-score.md).

## Wake behavior

- Wait for stable night sleep onset: at least 90 minutes of recorded night data
  and two matching observations at least five minutes apart.
- Target = onset + desired sleep duration + accepted recorded awake minutes.
- There is no fixed clock-time deadline and no automatically created clock-time
  fallback before onset is established.
- During the early window, choose an earlier alarm only when both the strap and
  the experimental model report fresh, aligned **Light or Awake**. Mixed pairs
  qualify too; no repeated Awake readings are required.
- Schedule that alarm at least **30 seconds** ahead, round upward to a minute,
  verify it by reading it back, and keep the primary alarm fixed for that night.
- After the primary alarm, fresh post-alarm activity indicating the strap is worn
  schedules a follow-up five minutes after the last verified alarm, repeating while worn.
  Late data schedules the next safe minute instead of a time that already passed.
- A fresh not-worn or charging record stops the sequence and cancels a pending
  bridge-owned follow-up. Missing HR, failed reads or stale data cannot create a follow-up.
  Removal detection depends on when the strap supplies its minute records.
  Follow-ups do not require the early sleep-stage gate; they are reminders to get up.
- A follow-up write still unverified when its time passes stops further reminders.
  Reboots retain the sequence, but installing this feature does not restart an already
  completed morning. Manual alarm controls and disabling smart wake stop reminders.
  The sequence retires at the next 18:00 night-arm boundary after the primary alarm;
  this never caps the sleep-duration target.
- If either source reports Deep or REM, is invalid, missing or stale, keep the full-target alarm.
- Preserve other Zepp alarms; manual bridge controls take precedence for the night.
- Settings, alarm ownership and dated night state survive restarts.

Factory defaults are **8.5 hours**, a **15-minute early window**, smart wake
**off**, and sleep monitoring **on**.
Saved settings override defaults and are not stored in this repository.
The early-window control accepts **0–60 minutes** in five-minute steps.
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
recorded awake time, strap sleep score, model stage, smart target, saved alarm and operation status.
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
6.5 minutes normally, 2.5 minutes in the early wake window or follow-up monitoring.
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

The starting model `20261003-v1` uses latest heart rate, five-minute mean and standard
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
The starting fit is the full-night fit, not a held-out fold.
Accepted adaptive versions keep these same causal features and the original scaling;
normalized adaptive inputs are clipped to ±8, and their learned coefficients persist locally.
Agreement at scheduling cannot establish the physiological sleep stage when
the strap vibrates 30–89 seconds later (plus any scheduling/connection delay).
The full sleep-duration alarm remains the fallback when the early gate is not met.

## Continuous learning on the ESP32

Learning and automatic checked updates default to **on** in this package.
Home Assistant exposes **Helio Model Learning**, **Helio Automatic Model Updates**,
and **Helio Model Learning Status**; no computer, scheduled job or Home Assistant
connection is needed for training, comparison or activation.
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
Private learned weights and training records are stored on the device, not in GitHub.

The model remains one half of the existing early-wake gate: the strap must also
report fresh, aligned Light/Awake inside the selected window.
The full-duration target, 30-second buffer, five-minute worn reminders,
manual overrides and alarm ownership rules are unchanged.

## Tests

Run `python tests/run.py` with Python 3.10+, a C/C++ compiler, OpenSSL with
`sect163r2` support, and the desktop dependencies.
The tests run locally without contacting or flashing any device.
They cover alarm ownership, protocol framing, sleep/awake accounting, data transfer
validation, dated sessions and legacy migration, DST, the Light-or-Awake gate, causal
features, worn follow-ups/removal/reboots, fixed Python/C++ inference parity and rolling log integrity.
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
