# Benchmarking and regression policy

Benchmarks are evidence, not product frame-rate promises. In particular, 100K sprites is a scaling observation rather than a 60 FPS acceptance target. Correctness runs and performance artifacts have separate purposes.

## Modes

The `validation` preset enables standard and synchronization validation. The `asan` preset enables MSVC AddressSanitizer. Those configurations are correctness evidence and are fingerprinted as baseline-ineligible.

Only `--preset benchmark` is eligible for promotion. It is optimized, validation-off, sanitizer-off, allocation-tracking-on, fixed at 640×360, and uses offscreen rendering to remove WSI/vsync effects. The allocation hook remains enabled because a measured allocation is an immediate invariant failure; its instrumentation state is part of the fingerprint.

Defaults are five runs, 300 warmup frames, and 3,000 measurement frames. `--quick` caps those values at three, 30, and 200. The full defaults can take many minutes across all 32 results; quick mode is the repeatable pre-merge sweep and was selected for the complete local v0.1 evidence run. Presented rendering is covered by validation smoke tests, not timing baselines.

```powershell
python tools/engine.py benchmark --suite all --counts 1000,10000,50000,100000 --quick --preset benchmark --output artifacts/benchmarks/candidate.json --json
python tools/engine.py promote-baseline artifacts/benchmarks/candidate.json local-rtx3060 --json
python tools/engine.py benchmark --suite all --counts 1000,10000,50000,100000 --quick --preset benchmark --baseline artifacts/benchmarks/baselines/local-rtx3060.json --output artifacts/benchmarks/repeat.json --json
```

Promotion is a separate explicit command. It refuses failing/non-optimized candidates and refuses to overwrite a named baseline unless `--replace` is also explicit. Benchmark artifacts and named local baselines live under ignored `artifacts/benchmarks`.

## Suites and invariants

- `world`: dense Transform2D+Sprite2D query, velocity integration, and wrapping at 1K/10K/50K/100K.
- `render`: raw static and moving offscreen renderer paths at every count.
- `scenario`: end-to-end world operations, extraction, queue/upload/submit, and GPU timestamps for static, moving, four-texture switching, mostly-visible, and mostly-invisible variants.
- `all`: runs and packages all three suite documents.

Every measured result must retain its exact world/render cardinality, finite checksum, zero tracked C++ heap allocations, zero capacity growth, and expected batch/draw/bind count. A frame-arena overflow must be zero when that suite actually instruments a frame arena; otherwise the value is `null` with an unavailable reason. Those invariants fail immediately without a statistical threshold. GPU selected-pixel correctness and Vulkan validation are separate CTest gates.

Memory reporting includes tracked C++ allocations, frame-arena overflow availability, VMA allocation count/occupied bytes, logical texture bytes, and total two-slot instance-buffer capacity. Process RSS is emitted as `null` with an unavailable reason because no portable dependency-free probe is implemented.

## Statistics

For each phase, samples are sorted and reported as average, median, p95, p99, minimum, maximum, sample count, and median absolute deviation (MAD). Percentiles use linear interpolation at rank `p/100 * (N-1)`. p99 is observational and is not a v0.1 regression gate.

Before comparison or promotion, the loader rejects non-standard JSON numbers, duplicate suites/results, empty result sets, missing memory/distribution fields, zero-sample distributions, non-finite values, mismatched fingerprint hashes, failed child results, and edited eligibility booleans. Baseline eligibility is recomputed from the recorded build and instrumentation fields. The comparator then checks result identity, ScenarioSpec/plan hashes where present, and a comparison key containing compiler/STL, configuration, OS/architecture/CPU/core count, GPU/device/driver/Vulkan version, mode/resolution, sample configuration, and instrumentation. Source commit or dirty marker is recorded but intentionally not an equality key—otherwise different revisions could never be compared. A mismatch produces `PERF_BASELINE_INCOMPARABLE`, never an absolute-time failure across hardware.

For comparable medians, provisional gates are:

- PASS below +7.5%, or when the absolute increase is within the noise floor.
- WARN at +7.5% when the increase exceeds max(2× baseline MAD, 0.01 ms CPU / 0.005 ms GPU).
- FAIL at +15% when the increase exceeds max(3× baseline MAD, the same absolute floor).
- WARN with `PERF_NOISE_TOO_HIGH` when baseline MAD itself exceeds max(7.5% of its median, the absolute floor); no strong regression conclusion is made for that metric.

A WARN returns exit code 0 but remains visible in the envelope. FAIL returns 1. An incomparable artifact returns 3. Synthetic tests pin PASS/WARN/FAIL/noise/incomparable behavior and injected allocation failure.

## v0.1 host evidence

The final optimized quick sweep on 2026-08-12 completed 32 results through 100K without a crash, invariant failure, allocation, growth, or overflow. At 100K, world query/integrate/wrap medians were 0.5334/0.5606/0.1072 ms. Raw moving renderer queue/upload/CPU-submit/GPU medians were 3.7794/0.3290/0.3838/0.9297 ms. Integrated moving frame/update/extraction/queue/GPU medians were 6.3652/0.7230/1.1987/3.9820/0.9351 ms, identifying queue formation as the visible-path bottleneck. The 99,988-culled case measured 1.5625 ms frame CPU and shifted its declared bottleneck to extraction.

The final artifact is `artifacts/benchmarks/v0.1-final-quick.json`. Against the explicitly promoted Phase 6 local baseline, its 172 comparable timing metrics produced 155 PASS and 17 WARN, with zero FAIL. Fourteen warnings identify noisy baseline distributions and three cross the provisional warning threshold. This supports keeping WARN non-fatal and retaining the current floors until several full-duration local baselines exist.
