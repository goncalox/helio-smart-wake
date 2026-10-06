# Personal sleep score v1

The aim is a personal 0–100 estimate of overnight sleep quality, using only the time the strap is worn for sleep.

This first version is an **experimental, transparent index**, rather than a validated predictor of felt sleep quality.

No daily ratings, daytime activity, morning-performance targets or always-on computer are required.

The ESP32 stores each night's first estimate, later revisions and matching Helio scores for comparison.

## What adapts

Personal reference values update from up to 28 days of previously completed, settled nights.

At least seven eligible prior nights are required for each personalized component.

The current night is excluded from its own baseline, and future nights never affect its first estimate.

A record is eligible only after its end is at least 12 hours old, a fresh strap read has occurred after that point, and its sleep/awake duration has been unchanged for at least an hour.

“Settled” describes this freshness/stability rule; it does not mean Zepp can no longer revise a record.

**Adaptation learns typical timing and physiology, not which nights actually felt good.**

There is no automatic weight fitting, Helio-score matching, supervised quality-label learning or claim that accuracy improves merely with time.

## Inputs and formula

The score accepts a completed night with a continuous valid sleep/awake timeline and a duration of 2–16 hours.

A gap, invalid record, open end or nap-only record cannot yield a new quality score.

Every non-Awake stage counts equally as asleep; Light/Deep/REM proportions and the separate learned alarm-stage classifier are not score inputs.

| Component | Weight | Calculation |
| --- | ---: | --- |
| Duration | 40 | `100 × min(asleep minutes / chosen target minutes, 1)` |
| Continuity | 30 | 75% sleep fraction within the recorded night + 25% fragmentation component |
| Timing | 10 | Circular distance from the average prior local sleep onset; first 30 minutes tolerated, then one point lost per six minutes |
| Overnight HR | 10 | Penalize elevations above the prior median, using robust variability with a 2 bpm minimum scale |
| Overnight movement | 10 | Penalize elevations in `log(1 + mean movement)` above prior nights, using robust variability with a 0.15 minimum scale |

Fragmentation starts at 100 and loses five points per awakening bout above three bouts per equivalent eight hours, clamped to 0–100.

Adjacent Awake segments form one bout.

The robust scale is `max(minimum scale, 1.4826 × median absolute deviation)`.

HR and movement each lose 15 points per positive robust standard deviation above the median, clamped to 0–100.

The duration target is the saved 6–10 hour sleep-target control, default 8.5 hours, frozen when that dated night's first score is recorded.

Only minute records **inside the recorded night** contribute HR and movement.

Removed/charging/unknown-wear samples and invalid HR are excluded, and overlapping reads cannot multiply observations or revise the first-arrival minute values.

HR requires at least 70% covered worn minutes, at least 120 valid HR minutes and at least half the night with HR; movement requires at least 70% covered worn minutes.

Each physiological baseline also requires seven prior nights meeting its coverage requirements.

There are no beat-to-beat intervals or raw PPG waveforms available here; minute-HR variability is not HRV.

All weights, tolerances, coverage cutoffs and stability rules above are **engineering heuristics**, not parameters established by a clinical validation study.

## Missing data and comparison

Unavailable components are omitted and the remaining weights are normalized.

The initial duration/continuity score has **70% component coverage**, which is explicitly exposed alongside the score.

Component coverage is the fraction of planned weights available, not a probability of accuracy or the fraction of overnight samples received.

The separate overnight-reading percentage appears in score details and audits.

Compare nights with matching model versions, component coverage and record maturity; a full score and a partial score do not have identical meanings.

A high number from our score, or agreement with Helio, is not evidence that our score is better.

The score has an independent formula and objective, but still shares the strap's sensors and sleep/awake measurements with Helio.

Quiet wakefulness, provisional strap records and missed readings can therefore affect both.

Without an independent outcome or reference measurement, we can examine stability, completeness and response to interruptions, but cannot establish superior sleep-quality accuracy.

This limitation is consistent with the need for external validation emphasized in the [AASM position statement on consumer sleep technology](https://aasm.org/advocacy/position-statements/consumer-sleep-technology/).

## Runtime and audit

Home Assistant receives **Helio Personal Sleep Score**, **Helio Personal Score Component Coverage**, **Helio Personal Score Status** and **Helio Personal Score Details**.

The existing screen continues to show Helio's score, and the smart-wake alarm logic is unchanged.

The model runs on the ESP32, retains 90 dated nightly summaries and keeps a three-day bounded minute cache; no computer is needed overnight.

Completed aggregate measurements survive minute-cache retirement.

A separate versioned, size-checked, CRC-protected NVS blob stores the complete model and cache, usually hourly or one minute after a new/revised score, only outside BLE operations, queued alarms, the early wake window and worn follow-ups.

A sudden power failure can lose uncommitted samples since the last save; a corrupt image resets only this module.

The first estimates are retained per stored onset; later estimates may change after corrected sleep records, newly received activity or newly eligible prior baselines.

A change to onset identifies a separate dated record rather than silently moving its original first estimate.

Versioned event kind 19 records the first/latest scores, both Helio comparison scores, inputs, component scores, coverage, version, target and stability state when a result materially changes.

The existing bounded onboard journal can eventually evict older raw/audit events, while the 90-night summary history remains separately stored.

Manual review can download the logs and run `python3 diagnostics/sleep_score_report.py PATH/TO/events.jsonl`.

The report compares recorded results only and does not fit our score to Helio.
