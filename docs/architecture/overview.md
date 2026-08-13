# Architecture overview

## Flow

Human/AI-authored JSON follows one of two compatible paths. ScenarioSpec 0.1 compiles to the original benchmark-focused `ExecutionPlan`. GameManifest plus SceneSpec 0.2 through 0.5 compiles to a `GamePlan` containing resolved assets, actions, scenes, collision/rule dispatch, UI, settings bindings, and transitions. Version 0.3 adds numeric logical grids and ordered typed Event–Action plans; 0.4 adds bounded lifecycle pools over scene-load-created entities; 0.5 adds bounded motion segments, solid/trigger interactions, direct contact lifecycle, linear motion, and numeric black-box verification. Older parsers and hashes retain their contracts. `GameRuntime` owns fixed 60 Hz simulation and hands interpolated sprite/UI values to the same `renderer2d` boundary.

No measured frame parses JSON, resolves strings, creates resources, changes world structure, grows project containers, or exposes GPU synchronization to authoring.

## Targets and allowed dependencies

| Target | Responsibility | Allowed project dependencies | Forbidden knowledge |
|---|---|---|---|
| `ai2d_foundation` | values, result/diagnostic, JSON writer, timers/stats, frame arena, allocation/build metadata | none | SDL, Vulkan, World, ScenarioSpec |
| `ai2d_world` | entities, fixed dense components/query, swept-AABB grid collision, guards, metrics | foundation | SDL, Vulkan, JSON, renderer resources |
| `ai2d_platform_sdl` | SDL window/input edges, PNG/WAV decode, audio voices, preference paths | foundation, SDL3 | game rules, batching, ScenarioSpec |
| `ai2d_renderer2d` | public 2D frame/camera/handles/metrics; private Vulkan/VMA backend | foundation, SDL3/Vulkan/VMA privately | World and ScenarioSpec internals |
| `ai2d_scenario` | JSON parse/validation, descriptors, dependencies, resolved plan | foundation, nlohmann/json privately | SDL, Vulkan, plugins/arbitrary code |
| `ai2d_runtime` | fixed-step orchestration, plan dispatch, extraction, metric aggregation | public APIs above | subsystem private storage |
| apps/tests | CLI/sandbox, assertions, integration | required public targets | bypassing public lifetime rules |

## Concrete data contracts

`EntityId` is `{index,generation}` with an invalid sentinel. `DenseStorage<T>` owns parallel dense component/entity arrays and a sparse index map; removal is swap-remove. Typed borrowed query views select the smallest participating store and sparse-probe the other. Any structural mutation during a live query returns `WORLD_STRUCTURAL_MUTATION_DURING_QUERY`.

Fixed components are `Transform2D`, `Velocity2D`, `Sprite2D`, `Collider2D`, and `EntityState2D`; all are compact values without strings, owning pointers, vectors, or virtual dispatch. `Transform2D` retains the previous position for render interpolation. `Sprite2D` stores a texture handle and normalized UV rectangle.

The operation descriptor table is the single source for v0.1 query, read/write sets, structural effects, legal phase, complexity, and allocation permission. v0.1 dispatches `integrate_velocity` and `wrap_bounds` once per system, then runs direct dense loops. GamePlan 0.3–0.5 separately uses a closed enum of built-in systems, events, comparisons, targets, and actions. The compiler resolves names into indices and validates order/limits; there is no runtime registry or expression evaluator.

`RenderFrame2D` contains copied visible/extraction values, not World references. Layer order is ascending. Items within a layer may reorder by texture; overlap requiring strict order uses separate layers. A batch key is pipeline/layer/texture; v0.1 has one sprite pipeline.

## Frame and memory boundaries

Normal phases are input-edge capture, zero-to-eight fixed 60 Hz updates, transition evaluation, interpolated extraction, queue build, submit, present, and metric finalization. A 0.3–0.5 fixed tick expires due pooled slots at its boundary, dispatches matching non-collision rules in declaration order, runs authored systems, performs solid response, then dispatches ordered collision/contact events. Real frame deltas clamp at 250 ms and excess catch-up time is reported and dropped. Headless tests request exact ticks or feed precompiled logical-action spans independent of wall-clock accumulation. World structure changes only when a scene is loaded or reset; pool acquisition only toggles and restores pre-existing entities.

Continuous collision uses a bounded uniform grid, swept AABB time-of-impact tests, deterministic entity ordering, and at most four impacts per dynamic body by default. In 0.5, solid integration records fixed motion segments; relative trigger sweeps compare static, kinematic, and dynamic segments and diff two sorted fixed-capacity contact sets. The collision core performs physical reactions; `GameRuntime` consumes numeric collision/contact events, captured endpoint positions, and lifecycle epochs. Logical-grid occupancy is separate preallocated byte storage used only by deterministic free-cell relocation. Each pool uses fixed-size FIFO and active-order arrays plus fixed expiration storage reserved during scene load; acquire, release, and oldest recycling do not create entities or grow containers.

UI is authored in a virtual 1280×720 coordinate system and rendered through the sprite pipeline. Font assets are PNG atlases with checked metadata; compilation rejects missing static glyphs and unknown state placeholders. PNG and WAV loading use SDL 3 core APIs with metadata-first per-file and 512 MiB aggregate decoded preflight. Images decode/upload incrementally, and disabled or unavailable audio skips WAV decoding. Settings resolve in the order CLI override, saved preference, manifest default and are replaced atomically.

Persistent/world/asset/frame lifetimes are tracked without a general allocator framework. Frame arena metrics are distinct from measured global C++ heap metrics. Capacity snapshots detect vector growth. The zero-allocation claim applies only after resource load, reservation, and warm-up with no resize/recreation.

## Vulkan boundary

The backend uses Vulkan 1.3 core dynamic rendering and synchronization2, two fence-protected frame slots, per-frame command resources/semaphores, VMA move-only buffers/images, a persistently mapped instance buffer, load-time textures/descriptors, an offscreen target/readback path, and delayed timestamp results. Build-validated SPIR-V is embedded in the executable, so packaged players do not depend on build-tree shader paths. Raw Vulkan handles and headers do not leave the private backend. Unsupported features produce `VK_DEVICE_UNSUPPORTED`; device loss is fatal structured shutdown only.

## Drift prevention

CMake public/private linkage is primary enforcement. `tools/check_architecture.py` checks forbidden includes, dependency headers, direct allocation APIs in hot-path directories, and narrowly documented bridge exceptions. Material divergence requires an ADR and evidence.
