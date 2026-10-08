# Personal sleep score v2

The aim is a personal 0–100 estimate of overnight sleep quality using only the time the strap is worn for sleep.

This is an **experimental, transparent index**, not a validated predictor of felt sleep quality.

No daily ratings, daytime activity, morning-performance targets or always-on computer are required.

The selected controller stores first estimates, subsequent corrections and matching Helio scores, with model versions kept distinct.

After handover, the separately maintained Home Assistant integration owns this calculation and imports all saved history; the ESP storage details below describe retained local mode.

## What changed from v1

Continuity now distinguishes one long awakening from brief awakenings with the same total awake time, and measures clustering within 30 minutes.

Heart rate includes adjacent-minute fluctuations and the change from the first to the last third of the night.

Movement includes how often activity occurs, how long consecutive bursts last and how many bursts occur, alongside average intensity.

Recent short nights add a small context component; the current night does not enter its own historical baseline.

The firmware preserves v1 historical scores and original first estimates during storage migration.

A v2 first estimate starts a separate comparison, and its audit retains the preceding v1 score, coverage, first estimate, date and Helio comparison values.

## Inputs and formula

A new score requires a completed main night with a continuous valid sleep/awake timeline and a duration of 2–16 hours.

Gaps, open ends, invalid records and nap-only records cannot yield a new quality score.

Every non-Awake stage counts equally as asleep; Deep/REM percentages, Helio's score and the learned alarm-stage classifier are not inputs or training targets.

| Component | Weight | Calculation |
| --- | ---: | --- |
| Duration | 30 | `100 × min(asleep minutes / chosen target minutes, 1)` |
| Continuity | 30 | 55% sleep fraction + 20% longest-awakening score + 15% awakening-frequency score + 10% clustering score |
| Timing | 10 | Circular deviation from average prior local onset; 30 minutes tolerated, then one point lost per six minutes |
| HR patterns | 15 | 40% HR-level score + 40% adjacent-minute-change score + 20% late-minus-early trend score, each compared with prior nights |
| Movement patterns | 10 | 30% average-intensity score + 30% active-minute-fraction score + 20% longest-burst score + 20% burst-frequency score, each compared with prior nights |
| Recent shortfall | 5 | `100 − 75 × average relative duration shortfall` across 3–7 eligible previous nights within eight days |

Missing components are omitted, and available weights are normalized to calculate the score.

With duration and continuity alone, component coverage is **60%** and their relative contributions are 50%/50%.

Coverage describes the available planned weights, not accuracy or the percentage of minute readings received.

The separate overnight-reading percentage is reported in details and audits.

The sleep target is the saved 6–10 hour control, default 8.5 hours, frozen when that dated night's first estimate is recorded and retained on upgrade.

### Interruption detail

The longest-awakening score starts at 100, tolerates five minutes, then loses three points per additional minute.

The frequency score starts at 100 and loses five points per awakening above three bouts per equivalent eight hours.

The clustering score starts at 100 and loses 15 points per bout above two beginning in any rolling 30-minute interval.

All component scores are clamped to 0–100.

Adjacent Awake segments form one awakening; splitting a segment cannot reset stability or increase the count.

Awake minutes in the last third are logged for review, without an extra penalty for natural morning wakefulness.

### Overnight patterns

Only minute samples inside the recorded night contribute HR and movement.

Removed/charging/unknown-wear samples and invalid HR are excluded; overlapping polls cannot multiply observations or change first-arrival minute values.

A missing minute breaks adjacent-HR comparisons and movement runs, rather than silently joining observations across a gap.

The HR pattern requires at least 70% recorded worn minutes, 120 valid HR minutes, adjacent pairs covering at least half the night, and at least 60% coverage plus 30 HR observations in both the first and last thirds.

Movement requires at least 70% recorded worn minutes.

A movement burst is a consecutive run of positive strap-reported activity intensity, with frequency normalized to eight hours.

These are activity proxies; ordinary turns, device noise and normal sleep-stage changes can affect them.

Adjacent-minute HR change is the mean absolute difference between valid consecutive minute HR readings.

Trend is mean HR in the final third minus mean HR in the initial third.

Minute-HR standard deviation is also logged for inspection, but is not scored separately.

None of these quantities is beat-to-beat HRV; raw PPG and beat intervals are unavailable here.

### Personalized comparison

Timing and each physiological comparison use at least seven eligible prior nights from the last 28 days.

Duplicate onset revisions do not count as additional nights, and another record from the current local night cannot become its own baseline.

Local night dates use the 18:00 boundary and the stored local onset, including the Lisbon clock offset.

A prior record is eligible only after its end is at least 12 hours old, a fresh read has occurred after that point, and its relevant sleep/awake timeline has been unchanged for an hour.

“Settled” describes this rule; it does not mean Zepp cannot revise the record again.

For each physiological feature, elevations above the prior median lose 15 points per positive robust standard deviation.

The robust scale is `max(floor, 1.4826 × median absolute deviation)`.

The floors are 2 bpm for mean HR, 1 bpm for mean adjacent-minute change, 3 bpm for trend, 0.15 for `log(1 + mean activity intensity)`, 0.02 for active-minute fraction, 0.25 for `log(1 + longest movement burst)` and 0.5 for bursts per eight hours.

Normal HR or movement variability is not automatically a defect; the index reacts to elevations relative to the person's recorded pattern.

Each prior physiological night must pass the same relevant coverage rules as the current night.

The recent-shortfall component uses each prior night's own frozen target; missing nights are excluded and extra sleep is not treated as proof that previous shortfall was repaid.

This is a small historical context adjustment, not a measured physiological sleep debt.

## Interpretation

All weights, thresholds, floors and stability rules above are **engineering heuristics**; they have not been established by a clinical validation study.

Adaptation learns usual timing and physiology, not which nights actually felt good.

A stable personal baseline can also represent an unhealthy pattern, so being typical is not proof of good sleep.

There is no automatic weight fitting, Helio-score matching or supervised quality-label learning.

Higher numbers or agreement with Helio do not establish greater accuracy.

Compare matching versions, component coverage and maturity; partial and full scores do not contain identical information.

Our formula is independent, but the sensors and sleep/awake measurements still come from the same strap as Helio.

Quiet wakefulness and incorrect strap timelines can therefore affect both scores.

Without an independent outcome or reference measurement, completeness, stability and sensitivity can be tested, but superior sleep-quality accuracy remains unproven.

The need for external validation is emphasized by the [AASM position statement](https://aasm.org/advocacy/position-statements/consumer-sleep-technology/).

The general relevance of regular timing and repeated short nights is described by [NHLBI sleep habits](https://www.nhlbi.nih.gov/health/sleep-deprivation/healthy-sleep-habits) and [sleep-deficiency effects](https://www.nhlbi.nih.gov/health/sleep-deprivation/health-effects), but these sources do not validate this formula or its weights.

## Runtime, storage and display

In local mode Home Assistant exposes **Helio Personal Sleep Score**, **Helio Personal Score Component Coverage**, **Helio Personal Score Status** and **Helio Personal Score Details**.

After handover use the integration’s **Experimental sleep score** and **Score coverage**, with full summaries kept in its private checkpoint.

The screen currently shows only Helio's score, centered at its original 54-pixel size.

Our experimental score is hidden from the screen while calculation, saved comparisons and eligible-night baseline updates continue automatically on the selected controller.

Alarm scheduling and the stage-learning controller are unchanged.

The model runs on the ESP32, retaining 90 dated summaries and a three-day bounded minute cache.

Completed pattern aggregates survive minute-cache retirement.

At the first v2 start, the ESP32 recovers available recent minute records from CRC-validated committed onboard logs, one batch per idle slice, without requiring a computer or altering the strap's data.

Older journal batches may already have rotated away; missing data cannot be reconstructed.

The next fresh sleep read recomputes the applicable nightly score from recovered measurements.

Recovery pauses for BLE operations, queued alarms, the early wake window and worn follow-ups.

The separate score blob is versioned, size-checked and CRC-protected, and migrates the v1 layout while preserving historical comparisons.

It is saved hourly or after a new/revised result, outside the same priority-sensitive periods, retaining storage space for alarms and settings.

A sudden power failure can lose uncommitted observations; a corrupt score image resets only this module.

Audit event kind 19 accepts the older 108-byte v1 format and a 176-byte v2 format containing all new patterns, historical context and prior-version comparison fields.

The onboard journal remains bounded and may evict older audits, while nightly summaries remain in the separate 90-slot history.

Manual review uses `python3 diagnostics/sleep_score_report.py PATH/TO/events.jsonl` after downloading logs; no computer is needed overnight.
