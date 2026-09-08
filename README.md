# AI2D Runtime

`game_runtime_dev` is a deliberately focused C++23 + SDL3 + Vulkan 1.3 engine for AI-authored 2D games. GameManifest/SceneSpec JSON compiles to a validated numeric plan, then runs on a fixed-component dense world and measurable instanced sprite renderer. It remains intentionally smaller than a general-purpose game engine.

The v0.6 slice adds bounded sprite animation, compile-time prefabs, versioned save slots, tile layers and integer fields, gamepad input profiles, deterministic camera rigs, anchored localized UI, streamed PCM WAV music, and preallocated particles. It builds on the v0.4 object pools and v0.5 moving contacts while preserving GameManifest/SceneSpec 0.2–0.5 behavior and hashes. [Content Foundations](samples/content_foundations/game.json) exercises the complete 0.6 slice without game-specific C++. The older [Breakout](samples/breakout/game.json), [Snake](samples/snake/game.json), [Grid Collector](samples/grid_collector/game.json), [Projectile Arena](samples/projectile_arena/game.json), [Timed Pickups](samples/timed_pickups/game.json), [Pool Siege](samples/pool_siege/game.json), [Contact Course](samples/contact_course/game.json), and source-blind samples remain fully declarative as well.

## Quick start (Windows reference host)

Python is the only tool that must be on `PATH`. The command wrapper finds the CMake/Ninja/MSVC tools bundled with Visual Studio 2022 and creates the developer environment.

```powershell
python tools/engine.py doctor --json
python tools/engine.py bootstrap --json
python tools/engine.py build --preset dev --fresh --json
python tools/engine.py test --preset dev --json
```

`bootstrap` downloads checksum-pinned LunarG Vulkan SDK 1.4.350.0 and Slang 2026.14.1 assets into ignored `artifacts/toolchain/`; it does not rely on a floating latest URL. The Vulkan SDK copy supplies headers, tools, and validation layers while the system GPU driver supplies the Vulkan runtime.

### CPU-only checks on Linux

With CMake 3.28+, Ninja, Python 3.10+ and a C++23 compiler installed (validated with GCC 13.3), run:

```sh
python tools/engine.py test --preset gpu-off --json
```

`doctor` discovers the native compiler or the executable and arguments selected by `CXX`. It still reports unavailable GPU tools separately. The portable SDK bootstrap and playable package remain Windows-specific. Legacy GameSpec 0.2–0.5 plan hashes include installation paths; their compatibility tests normalize paths in a test-only copy. GameSpec 0.6 retains its relocation-independent runtime hash.

Other configured build modes:

```powershell
python tools/engine.py test --preset release --json
python tools/engine.py test --preset asan --json
python tools/engine.py test --preset validation --json
python tools/engine.py test --preset gpu-off --json
python tools/engine.py build --preset benchmark --json
python tools/check_architecture.py --root . --json
```

Scenario/runtime/compiler commands:

```powershell
python tools/engine.py validate tests/fixtures/scenarios/valid/static_sprites.json --preset dev --json
python tools/engine.py inspect plan tests/fixtures/scenarios/valid/moving_sprites.json --preset dev --json
python tools/engine.py run tests/fixtures/scenarios/valid/moving_sprites.json --frames 600 --headless --preset dev --json
python tools/engine.py benchmark --suite all --counts 1000,10000,50000,100000 --json
python tools/engine.py profile tests/fixtures/scenarios/valid/moving_sprites.json --frames 600 --preset release --json
```

GameManifest 0.2–0.6 commands:

```powershell
python tools/engine.py validate samples/pool_siege/game.json --preset dev --json
build/dev/bin/ai2d_cli.exe game inspect samples/pool_siege/game.json --json
python tools/engine.py run samples/snake/game.json --headless --frames 180 --input-script samples/snake/input/grow-and-turn.json --preset dev --json
python tools/engine.py verify samples/pool_siege/game.json --test-script samples/pool_siege/tests/smoke.json --repeat 2 --preset dev --json
python tools/engine.py verify samples/contact_course/game.json --test-script samples/contact_course/tests/contact-lifecycle.json --repeat 2 --preset dev --json
python tools/engine.py verify samples/content_foundations/game.json --test-script samples/content_foundations/input/verify.json --repeat 2 --preset dev --json
python tools/engine.py package samples/pool_siege/game.json --output artifacts/packages --zip --json
```

The interactive run opens a native Vulkan window. Breakout uses arrows or A/D, Space/Enter, Tab, mouse, and Escape. Snake and Grid Collector use cardinal arrows. Projectile Arena fires with Space/Up. Timed Pickups moves with Left/Right or A/D. Pool Siege fires on three lanes with Down/Space/Up. Content Foundations uses arrows/A/D, Space, Q/E/R/F, Tab, and a gamepad profile. Regenerate the checked-in procedural sprites, 5×7 ASCII font, and procedural WAV with `python tools/generate_sample_assets.py`; they use no external image or music generation.

Headless and offscreen runs require an explicit positive `--frames N`; omitting it is a structured input error instead of an unbounded loop. In JSON mode stdout contains exactly one JSON document. Configure/build/test and human runtime logs go to stderr. Exit codes are 0 success, 1 failed test/validation/benchmark, 2 invalid command/input, 3 unavailable capability/tool, and 4 internal failure.

## Design constraints

Public execution plans are revalidated at the runtime boundary, even if callers mutate them after compilation. World does not know Vulkan; renderer does not read World; Vulkan types remain private. ScenarioSpec v0.1 remains the allocation-measured benchmark path. The rule runtime is a closed typed instruction set: there is no scripting VM, arbitrary component query, generic ECS, runtime entity creation, or gameplay C++ plug-in.

The complete boundary and formats live in [the architecture overview](docs/architecture/overview.md), [0.6 content-foundations contract](docs/game-spec-v0.6.md), [0.5 motion/contact contract](docs/game-spec-v0.5.md), [0.4 pool contract](docs/game-spec-v0.4.md), [0.3 contract](docs/game-spec-v0.3.md), [0.2 contract](docs/game-spec-v0.2.md), [0.5→0.6 migration guide](docs/migration-0.5-to-0.6.md), [ScenarioSpec contract](docs/scenario-spec.md), and [ADRs](docs/adr/).

v0.6 validation and release-gate evidence is recorded in the [v0.6 implementation report](docs/reports/v0.6-implementation-report.md).

The [2026-09-08 reliability review](docs/reports/reliability-review-2026-09-08.md) records subsequent fixes, CPU validation and remaining priorities.
