# ADR-0006: reproducible benchmark and regression policy

- Status: accepted
- Date: 2026-08-12

## Context

Absolute frame-rate gates are invalid before a local baseline, and presented paths contain WSI/vsync noise.

## Decision

Prioritize optimized, validation-off, sanitizer-off fixed-resolution offscreen runs. Fingerprint scenario/plan/build/compiler/OS/CPU/GPU/driver/mode/instrumentation. Report average/median/p95/p99/min/max and MAD. Compare only matching fingerprints, with provisional WARN +7.5% and FAIL +15% after absolute/noise floors. Correctness/allocation/draw invariants fail immediately. Baselines change only by explicit promote.

## Alternatives considered

Cross-machine absolute comparison, p99 gating, one-process timing, and automatic baseline overwrite were rejected as noisy or unsafe.

## Evidence and consequences

The host includes virtual-display/overlay layers, reinforcing offscreen priority. Initial thresholds are provisional until repeated variance is measured.

## Revisit trigger

MAD/noise approaches the thresholds, or repeated stable regressions evade the policy.
