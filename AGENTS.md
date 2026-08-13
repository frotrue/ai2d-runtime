# Repository rules

## Purpose and current boundary

This repository is a C++23 + SDL3 + Vulkan 1.3 experiment for a small, high-performance, AI-first 2D runtime and declarative game compiler. The v0.1 ScenarioSpec benchmark path remains supported and continues to test these hypotheses:

1. A versioned declarative ScenarioSpec can express static and moving sprite workloads without scenario-specific gameplay C++.
2. Generational entities, dense storage, preallocated extraction, CPU culling, batching, and instanced drawing produce measurable, explainable scaling.
3. After warm-up, project-owned measured-frame heap allocations, frame-arena overflows, and container growth can remain zero.
4. Invalid specs/handles, memory regressions, batch fragmentation, Vulkan validation failures, and unsupported devices have stable structured diagnostics.
5. Same-environment benchmark comparison can distinguish noise from a practical regression.

v0.2 additionally permits only the closed GameManifest/SceneSpec slice recorded in ADR-0009: fixed action bindings, bounded integer state, fixed-step scenes, grid-placed spawn groups, swept-AABB collision and fixed reactions, startup PNG/font/WAV loading, bounded audio voices, virtual-resolution panels/text/buttons, FPS settings, and scene transitions. v0.3 adds only the bounded typed rules, logical grids, grid movement, transform-chain following, active reserves, deterministic free-cell relocation, input scripts, and packaging recorded in ADR-0010. v0.4 adds only the scene-load-created bounded object pools, deterministic acquisition/release/recycling, fixed-tick lifetimes, and pool snapshots recorded in ADR-0011. v0.5 adds only bounded motion segments, explicit kinematic linear motion, solid/trigger interactions, typed contact begin/end, and numeric black-box verification recorded in ADR-0012. New gameplay primitives require an explicit reviewed built-in operation or action; do not add an arbitrary gameplay C++ escape hatch.

Do not add a generic ECS, archetypes, runtime component registration/reflection, generic RHI/render graph, scripting/VM, gameplay extension API, full IR/optimizer/code generation, job system/thread pool, general rigid-body physics, animation graphs, editor/ImGui, hot reload, a generic asset pipeline, bindless/GPU-driven rendering, 3D, networking, or speculative extension interfaces.

## Dependency direction

`foundation <- world <- runtime`

`foundation <- scenario <- runtime`

`foundation <- renderer2d <- runtime`

`foundation <- platform_sdl <- runtime`

World and scenario never include SDL or Vulkan. World owns only fixed components and the concrete bounded AABB collision core; it does not own game reactions, assets, UI, or scene orchestration. World never knows GPU resources; `Sprite2D` stores only an opaque `TextureHandle`. Renderer public headers never expose Vulkan types, and renderer never reads `World` directly. Vulkan headers stay in `src/renderer2d/vulkan`; SDL headers stay in `src/platform/sdl` except the documented SDL/Vulkan surface bridge. JSON parsing remains private to scenario. CMake target visibility is the primary boundary; `tools/check_architecture.py` is a second line of defense.

## C++ and hot-path rules

Use value semantics, `std::expected`, `std::span`, `std::string_view`, `std::optional`, move-only ownership, and source locations where they clarify ownership or errors. Project source must not contain raw owning pointers, direct `new`/`delete` outside the isolated test allocation hook, shared ownership without written evidence, global mutable state, singleton/service locators, per-entity virtual dispatch, hot-path `std::function`, runtime reflection/type registries, per-frame string/JSON lookup, or `unordered_map` iteration in entity loops.

Reserve scenario/world/collision/UI/render capacities before warm-up. During measured steady-state frames, tracked C++ heap allocation, frame-arena overflow, and container capacity growth are correctness failures. Scene loads, explicit resets, resize/recreation, and startup asset decode are structural phases outside that measured interval. Arena allocations and heap allocations are separate metrics. Never claim driver/external allocation coverage.

## Official commands and evidence

Use `python tools/engine.py` for `bootstrap`, `doctor`, `build`, `test`, `run`, `validate`, `inspect`, `benchmark`, and `profile`. JSON mode writes one valid JSON document (or explicitly requested JSONL) to stdout; human logs go to stderr. Every subsystem change needs a correctness test and relevant metric.

Never claim "optimized", "fast", "zero allocation", or "validation clean" without current measured evidence. Never ignore sanitizer or Vulkan validation failures. Unexecuted checks are `SKIPPED` or `UNAVAILABLE`, never `PASS`. Do not silently accept or overwrite a benchmark baseline.

## Dependencies and architecture decisions

Dependencies require an exact version/commit or artifact hash, official source, license, role, rationale, alternative, and update policy in `docs/dependencies.md`. Do not use floating branches. Keep third-party warnings separate from project warnings. Record material architecture changes in `docs/adr`; include context, decision, alternatives, evidence, consequences, and revisit trigger.
