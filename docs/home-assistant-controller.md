# Home Assistant owns wake decisions

The native **Helio smart wake routine** automation owns wake decisions on Home
Assistant: target time, awake compensation, early Light/Awake agreement, safety
buffer, reminders and removal/manual/disabled handling.
The `helio_smart_wake` integration supplies sleep observations, stage inference,
learning/validation, personal scoring and explicit durable alarm services.
It cannot create an alarm from a sleep record, periodic update or model result.
The ESP32 authenticates with the strap, reads records, maintains a bounded replay
journal and performs explicitly requested alarm writes with readback verification.
The strap executes a successfully saved alarm locally.
The Mac is used only for development and is not a runtime dependency.

## Same algorithms, different owner

`homeassistant/native/build.py` compiles the existing `helio_smart.cpp` policy
against a process adapter and includes the same `smart_wake.h`, `stage_model.h`,
`adaptive_model.h` and `sleep_score_model.h` algorithms used by the firmware.
The worker is a local subprocess of the integration, not an additional network
service or scheduled Mac task.
The runtime worker accepts `OBSERVE` to process data and models; it has no runtime
command for making wake decisions.
The Python integration persists each explicit instruction before BLE dispatch;
only a caller of `set_alarm` or `cancel_alarm` can create a command.
The retained C++ wake policy remains available for ESP local mode and regression
comparisons; it is not called by the HA observation runtime.
All algorithm source is in this repository; the platform-specific executable is
generated and excluded from Git.
The native build currently supports the configured `Europe/Lisbon` timezone;
changing zones requires updating both the worker and firmware and testing DST.

## Installation

1. Install the updated ESPHome package and retain its encrypted API configuration.
   The new firmware defaults to **ESP32 ownership**, preserving existing operation.
2. Build the worker on a Linux system matching the HA host architecture, using a
   C++17 compiler and Python 3:

   ```sh
   python3 homeassistant/native/build.py /tmp/helio-engine --static
   ```

   A static musl build is supported on the HA SSH app's Alpine runtime.
   Build dependencies can be installed in that app; Docker access and disabling
   its protection mode are unnecessary.
3. Copy `homeassistant/custom_components/helio_smart_wake/` into HA's
   `/config/custom_components/helio_smart_wake/` and copy the resulting executable
   there as `helio-engine`, with executable permissions.
4. Restart HA and add **Helio Smart Wake** in Settings → Devices & services.
   Select the existing ESPHome entry; its API key is read internally, never copied
   into this integration's configuration or requested in the form.
5. It imports the complete saved model/score/night state and archives all remaining
   committed ESP journal batches while following new pending data.
   It starts in shadow mode; no alarms are dispatched by HA.
6. Run the host regression suite and inspect imported history, current readings and
   shadow comparisons before invoking `helio_smart_wake.activate`.
   Activation re-imports the current ESP snapshot and transfers ownership while idle.
7. Create the restoring text helper `input_text.helio_wake_session` (maximum 255
   characters, no initial value) through HA's helper UI/API.
   Import `homeassistant/automation/helio-smart-wake.json` through HA's automation
   configuration API, initially disabled, and enable it after checking the new
   observation, instruction and connectivity entities.
   Its JSON is the native automation configuration, not a separate scheduler; all
   rules appear in HA's automation editor and execution traces.
8. Use the **Helio Smart Wake** device's sleep target, wake window, smart wake,
   model learning and automatic-update controls afterwards.
   Previous ESP policy controls are inactive when HA owns decisions and may be hidden;
   strap readings, manual alarm buttons, brightness and the screen remain on the ESP.

The repository's nested integration directory is a manual installation layout;
it is not currently a HACS package.
Never install a Mac executable on the Linux HA host.

## Durable data and transport

- HA Store contains the active models, up to 90 personal scored nights, learner
  examples, settings, replay cursor, duplicate-event identities and explicitly
  accepted alarm instructions/receipts.
- The restoring `input_text.helio_wake_session` helper contains the automation's
  dated primary time, early-selection lock, reminder state and manual generation.
  An accepted instruction stores a copy of caller state before BLE work, allowing
  the automation to recover a crash between service acceptance and helper update.
- `/config/helio_history/<entry_id>/` stores the initial snapshot, lossless committed
  raw batches, the latest pending snapshot and daily command/error audit files.
  Changes in champion/candidate versions or validation counts also retain a full
  private model checkpoint, including coefficients and their training context.
  Raw history is retained; there is no automatic deletion policy.
  Include this directory and HA Store in Home Assistant backups.
- Transfers require contiguous chunks, valid size/schema and CRC; a partial or
  corrupted transfer is never accepted.
- Pending events and later committed copies have the same timestamp/uptime/kind/CRC
  identity and are ingested once, including after HA restarts.
- Gaps caused by the ESP's bounded journal being overwritten are recorded explicitly;
  the integration does not invent missing stages or wear evidence.
- Decisions use the original device acquisition timestamp, not the later download
  timestamp, so replay cannot make old records appear fresh.
- Live decisions and acknowledgements run separately from bounded journal replay.
  A slow history transfer cannot hold the decision lock or delay command dispatch.
  Older replay events cannot replace newer live stage or wear observations.
- Polling unchanged state does not rewrite the large model checkpoint.
- The ESP polls normally every five minutes; the automation explicitly requests
  minute reads near the target,
  in the early window and while following wear after an alarm.
  A short fast-read lease permits minute activity refreshes only while HA requests it.

## Explicit integration actions

- `helio_smart_wake.set_alarm`: future dated `when` (Unix seconds or timestamp),
  stable `request_id`, optional caller `context`; returns `command_id` and receipt
  status. Times must be exact minutes, at least 30 seconds ahead and less than
  24 hours away. Repeating an identical request is idempotent; conflicting reuse
  is rejected. Acceptance is not readback verification or physical vibration.
- `helio_smart_wake.cancel_alarm`: stable `request_id` and optional context;
  cancels only the verified bridge-owned alarm.
- `helio_smart_wake.refresh`: requests a rate-limited read; `frequent: true`
  requests a short minute-read lease without changing an alarm.

`sensor.helio_smart_wake_sleep_observations` exposes stable/raw onset and awake
minutes, band/model stage, sample/acquisition times, wear evidence and dated
observation bounds.
`sensor.helio_smart_wake_alarm_instructions` exposes pending work, the last caller
instruction, verified epoch and manual generation.
`binary_sensor.helio_smart_wake_bridge_connected` reports API connectivity.
The integration fires `helio_smart_wake.alarm_verified` only after readback;
it does not claim that vibration was observed.

Disabling the automation prevents new wake decisions; data and learning continue,
and already accepted instructions may still finish or retry.
Use the smart-wake switch to ask the automation to cancel a future owned alarm
before disabling it.
No lights, music or other wake outputs are configured by this project.

## One owner and failure behaviour

The ESP persists its owner independently of the API connection and starts with
that owner after a reboot.
With HA ownership selected, local wake ticks and learning/scoring updates are
disabled; local raw acquisition, logs, NTP, BLE verification and display continue.
There is no automatic second controller or fallback takeover during an HA outage.
The last verified alarm remains on the strap; an outage can prevent new adjustments
and follow-ups, which is reported as unavailable rather than success.

Every remote alarm request has a monotonic command ID, dated minute, expiry,
previous verified epoch and operation type.
The ESP persists intent before BLE work, rejects conflicting/stale IDs or expired
times, reuses owned slots and verifies the alarm list before confirming success.
HA retries the same durable command after a lost response and adopts a matching
verified result after reboot; it does not infer vibration from an elapsed alarm.
An unverified attempt that has passed is not rearmed for tomorrow.
Manual bridge controls or detected external changes pause the dated night.
Cancellation refuses to remove a different manually changed alarm.

The ESP owner selector permits explicit recovery to its retained local controller;
this pauses the current local night and resumes on the next dated-night boundary.
HA stops dispatching when the ESP does not report HA ownership.
It does not silently overwrite that explicit choice.
The retained ESP model history is the handover snapshot, not a continuously synced
copy of later HA learning; reverse model transfer is not implemented.

## Validation

Run `python3 tests/run.py` without contacting either device.
The suite covers both the existing firmware policy and the native worker, including
the dual stage gate, missing/stale/future data, minute rounding, full-target retention,
follow-ups, removal, manual overrides, restart, corrupted storage and journal data,
persist-before-send, failed saves, idempotent retries and lost acknowledgements.

For the first HA-owned night, check raw arrival/acquisition times, stable onset,
awake compensation, target and gate decisions in the native automation traces,
plus `alarm_requested`/`alarm_verified` receipts in HA's private audit and entity
history.
The synthetic automation tests execute the generated configuration and cover
freshness, dual-stage agreement, manual/off/removal behaviour, lost helper writes,
uncertain commands, five-minute reminders and primary targets beyond 18:00.
Physical vibration and how waking felt still require the user's observation.
No retrospective comparison proves that the heuristic personal score is better
than Helio or that a provisional strap stage was the final stage.
