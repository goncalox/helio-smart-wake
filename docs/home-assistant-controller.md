# Home Assistant owns wake decisions

The `helio_smart_wake` custom integration runs sleep-stage inference, candidate
learning/validation, personal sleep scoring, wake decisions and preferences on
the Home Assistant host.
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
The Python integration persists its complete checkpoint before sending any alarm
intent; the worker's in-memory persistence hooks alone do not authorize a write.
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
7. Use the **Helio Smart Wake** device's sleep target, wake window, smart wake,
   model learning and automatic-update controls afterwards.
   Previous ESP policy controls are inactive when HA owns decisions and may be hidden;
   strap readings, manual alarm buttons, brightness and the screen remain on the ESP.

The repository's nested integration directory is a manual installation layout;
it is not currently a HACS package.
Never install a Mac executable on the Linux HA host.

## Durable data and transport

- HA Store contains the active models, up to 90 personal scored nights, learner
  examples, dated session/follow-up state, settings, replay cursor, duplicate-event
  identities and outstanding alarm intent.
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
- The ESP polls normally every five minutes; HA requests minute reads near the target,
  in the early window and while following wear after an alarm.
  A short fast-read lease permits minute activity refreshes only while HA requests it.

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
awake compensation, target, gate decisions and `alarm_requested`/`alarm_verified`
events in HA's audit and entity history.
Physical vibration and how waking felt still require the user's observation.
No retrospective comparison proves that the heuristic personal score is better
than Helio or that a provisional strap stage was the final stage.
