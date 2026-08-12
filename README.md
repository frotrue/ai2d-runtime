# AI2D Runtime

`game_runtime_dev` is a deliberately focused C++23 + SDL3 + Vulkan 1.3 engine for AI-authored 2D games. GameManifest/SceneSpec JSON compiles to a validated numeric plan, then runs on a fixed-component dense world and measurable instanced sprite renderer. It remains intentionally smaller than a general-purpose game engine.

The v0.3 slice adds bounded ordered Event–Action rules, logical grids, exact fixed-tick grid movement, transform-chain followers, deterministic free-cell placement, logical-action input scripts, and reproducible Windows packages. It preserves the v0.1 ScenarioSpec path and accepts GameManifest/SceneSpec 0.2 unchanged. [Breakout](samples/breakout/game.json), [Snake](samples/snake/game.json), and [Grid Collector](samples/grid_collector/game.json) are fully declarative; they contain no game-specific C++.

## Quick start (Windows reference host)

Python is the only tool that must be on `PATH`. The command wrapper finds the CMake/Ninja/MSVC tools bundled with Visual Studio 2022 and creates the developer environment.

```powershell
python tools/engine.py doctor --json
python tools/engine.py bootstrap --json
python tools/engine.py build --preset dev --fresh --json
python tools/engine.py test --preset dev --json
```

`bootstrap` downloads checksum-pinned LunarG Vulkan SDK 1.4.350.0 and Slang 2026.14.1 assets into ignored `artifacts/toolchain/`; it does not rely on a floating latest URL. The Vulkan SDK copy is used for headers, tools, and validation layers while the system GPU driver continues to provide the Vulkan runtime.

Other configured build modes:

```powershell
python tools/engine.py build --preset release --json
python tools/engine.py test --preset asan --json
python tools/engine.py build --preset validation --json
python tools/engine.py build --preset benchmark --json
python tools/engine.py test --preset gpu-off --json
python tools/check_architecture.py --json
```

Runtime/compiler commands:

```powershell
python tools/engine.py validate tests/fixtures/scenarios/valid/static_sprites.json --preset dev --json
python tools/engine.py inspect plan tests/fixtures/scenarios/valid/moving_sprites.json --preset dev --json
python tools/engine.py inspect diagnostics-schema --preset dev --json
python tools/engine.py run tests/fixtures/scenarios/valid/moving_sprites.json --frames 600 --headless --preset dev --json
python tools/engine.py benchmark --suite all --counts 1000,10000,50000,100000 --json
python tools/engine.py promote-baseline artifacts/benchmarks/candidate.json local-baseline --json
python tools/engine.py profile tests/fixtures/scenarios/valid/moving_sprites.json --frames 600 --preset release --json
```

GameManifest 0.2/0.3 commands:

```powershell
build/dev/bin/ai2d_cli.exe game validate samples/breakout/game.json --json
build/dev/bin/ai2d_cli.exe game inspect samples/breakout/game.json --json
build/dev/bin/ai2d_cli.exe game run samples/breakout/game.json --headless --frames 600 --json
build/dev/bin/ai2d_cli.exe game run samples/breakout/game.json
python tools/engine.py validate samples/snake/game.json --json
python tools/engine.py run samples/snake/game.json --headless --frames 180 --input-script samples/snake/input/grow-and-turn.json --json
python tools/engine.py package samples/snake/game.json --output artifacts/packages --zip --json
```

The interactive commands open a native Vulkan window. Breakout uses arrows or A/D, Space/Enter, Tab, mouse, and Escape. Snake and Grid Collector use cardinal arrows (A/D are also bound horizontally), Space/Enter to restart, and Escape to quit. Regenerate the checked-in procedural v0.3 sprites and 5×7 ASCII font with `python tools/generate_sample_assets.py`; they do not rely on external image or music generation.

Headless and offscreen runs are non-interactive, so they require an explicit positive `--frames N`; omitting it is a structured input error instead of an unbounded loop.

In JSON mode stdout contains exactly one JSON document. Configure/build/test and human runtime logs go to stderr. Exit codes are 0 success, 1 failed test/validation/benchmark, 2 invalid command/input, 3 unavailable tool/capability, and 4 internal failure.

## Design constraints

Public execution plans are revalidated at the runtime boundary, even if callers mutate them after compilation. World does not know Vulkan; renderer does not read World; Vulkan types remain private. ScenarioSpec v0.1 remains the allocation-measured benchmark path. The rule runtime remains a closed typed instruction set—there is no scripting VM, arbitrary component query, generic ECS, or gameplay C++ plug-in. The complete boundary and formats live in [the architecture overview](docs/architecture/overview.md), [0.3 quickstart](docs/game-spec-v0.3.md), [0.2 contract](docs/game-spec-v0.2.md), [migration guide](docs/migration-0.2-to-0.3.md), [ScenarioSpec contract](docs/scenario-spec.md), and [ADRs](docs/adr/).

v0.3 validation and review evidence is recorded in the [v0.3 implementation report](docs/reports/v0.3-implementation-report.md).
