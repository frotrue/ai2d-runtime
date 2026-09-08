# Reliability review — 2026-09-08

Reviewed base: `c63154fa96e2f10a56ef904dd08ae7d460ee6433` (v0.6).

## Assessment

The engine has a coherent, deliberately bounded design: authored JSON resolves to numeric plans, public plans are revalidated, subsystem boundaries are explicit, and allocation and sample-behavior checks are already substantial. The most useful immediate work was to repair reproducible foundation defects and make the existing CPU checks usable outside the original Windows checkout.

This review covered repository contracts, build/tooling, foundation and world storage, fixed-step handling, compiler identity, and existing sample regressions. It was not a fresh visual or GPU-driver audit.

## Fixed findings

| Area | Reproduced problem | Change |
|---|---|---|
| Frame arena | Aligning only the offset ignores the byte vector's base address. Over-aligned allocations can return misaligned pointers; an impossible alignment request can incorrectly succeed. | Align the actual pointer with `std::align`, account for padding within capacity, and preserve state on failure. Cover typed page alignment, empty storage, capacity exhaustion and measured allocations. |
| Dense component storage | Inserting another generation of an occupied entity index overwrites the sparse mapping, making the original live component unreachable. | Reject generation conflicts before mutation; allow reuse after the original component is removed. |
| Fixed-step clock | A valid finite configuration can produce a quotient outside `uint64_t`; the conversion triggers UBSan and can return zero catch-up ticks. Adding two finite durations can also overflow the accumulator. | Bound the floating quotient before integer conversion and reject non-finite accumulated/dropped time without changing the clock state. |
| Compiler diagnostics | `doctor` searched only for MSVC `cl`, reporting a missing compiler on a host with GCC installed. | Discover native `c++`, GCC or Clang, respect explicit Unix `CXX`, and request the correct version output. Preserve Windows MSVC discovery. |
| GCC CPU build | Implicit signed-byte conversions and GPU-only helpers stopped the warnings-as-errors build. | Make byte conversions explicit and compile GPU-only helpers conditionally. UTF-8 output is covered. |
| Test portability | Legacy plan goldens included one installation's absolute paths, and a mocked Windows packaging test still hit the real non-Windows gate. | Normalize paths only in copied test plans, retain seven legacy plan goldens plus source-hash/repeat checks, and explicitly test the unavailable package response before mocking the package host. Production plan hashing is unchanged. |
| Test JSON metrics | CTest 4.4's successful summary omits `0 tests failed`, so the wrapper dropped all test counts from JSON. | Parse both summary formats and test successful and failing outputs. |

The standalone reproduction before these changes accepted an impossible arena alignment, lost the original component after a conflicting insert, and reported an out-of-range floating-to-integer sanitizer error. The same reproduction after the fixes rejects both invalid operations and returns the configured eight catch-up ticks.

## Current evidence

Host: Linux x86_64, GCC 13.3, CMake 4.4.3, Ninja 1.13.2, Python 3.12.13. Dependencies use the repository's pinned commits.

| Check | Result |
|---|---|
| `python tools/engine.py test --preset gpu-off --json` | PASS: 146/146, warnings-as-errors enabled. JSON reports total/passed/failed counts. |
| Architecture check (included in CTest) | PASS: 75 files, 0 violations. |
| Targeted frame-arena, fixed-step and world unit tests with ASan, UBSan and float-cast-overflow instrumentation | PASS: 20 cases, 438 assertions. The relevant project sources and test translation units were instrumented; Catch2's existing static libraries were reused. |
| World benchmark at 1K/10K/50K/100K entities | PASS: 4/4; tracked C++ heap allocations, allocated bytes and container capacity growth all 0 in measured intervals. |
| Native compiler diagnostic | Correctly reports GCC 13.3; GPU capability remains unavailable. |
| Windows player/package execution, Vulkan rendering/validation | UNAVAILABLE on this host; existing Windows report is historical evidence only. |
| LeakSanitizer | UNAVAILABLE: process inspection through `/proc` fails in this environment. Targeted ASan/UBSan checks ran with `ASAN_OPTIONS=detect_leaks=0`; no leak-clean claim is made. |

The benchmark command was:

```sh
python tools/engine.py benchmark --suite world --counts 1000,10000,50000,100000 --runs 1 --warmup 10 --frames 60 --preset gpu-off --output artifacts/benchmarks/reliability-world.json --json
```

This is a debug correctness/memory check, not evidence of a speed improvement or a replacement performance baseline. The world benchmark does not instrument a frame arena; the dedicated arena tests cover its allocation and overflow behavior. Existing benchmark baselines were not changed.

## Remaining priorities

1. Split the roughly 8,700-line game compiler and 5,300-line game runtime along existing parsing/validation, save and presentation responsibilities, preserving numeric plans and subsystem boundaries. The current concentration makes future feature changes harder to review. A staged extraction with the existing compatibility suite is safer than adding new extension interfaces.
2. Automate CPU checks and retain a Windows GPU validation/package gate. The Linux checks now execute successfully, but they cannot substitute for interactive input, audio, Vulkan synchronization and packaged-player validation.
3. Treat legacy 0.2–0.5 path-dependent hashes as an explicit compatibility constraint. A future change to their runtime identity needs a versioned migration decision; this repair deliberately confines normalization to tests.

No new gameplay primitive, schema version, dependency or architecture extension was introduced.
