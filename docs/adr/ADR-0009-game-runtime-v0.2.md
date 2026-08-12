# ADR-0009: Closed declarative game runtime for v0.2

- Status: accepted
- Date: 2026-08-12

## Context

v0.1 proved the numeric ScenarioSpec, dense World, extraction, and Vulkan sprite path with benchmark workloads, but it could not express a small playable game. The next decision was whether collision, audio, UI, file assets, and scene management fit the AI-first compiler experiment, or whether adding them would turn the project into a conventional general-purpose engine. Animation and an editor were considered at the same boundary.

The useful seam is not the number of engine features; it is whether authoring remains versioned, bounded, declarative, compiled before execution, and observable through stable diagnostics. A Breakout workload supplies concrete contracts for collision, action edges, state, assets, UI, audio, and scene transitions. It does not supply evidence for a general physics API, animation graph, plugin ABI, scripting language, or editor data model.

## Decision

v0.2 adds GameManifest and SceneSpec 0.2 as a second, closed authoring path. They compile into a numeric `GamePlan`; runtime dispatch remains limited to three built-in systems and five built-in collision reactions. Breakout is validation content, not a privileged C++ code path.

The slice includes:

- fixed keyboard/mouse action bindings and bounded integer state;
- fixed 60 Hz scenes with bounded catch-up and explicit transitions;
- grid placement and the existing fixed World components plus concrete swept-AABB collision;
- startup PNG/font/WAV loading, texture upload, and bounded audio voices;
- virtual-resolution panels, text, buttons, and compiled integer placeholders;
- FPS settings with strict optional load and atomic replacement;
- reset-or-retain scene re-entry semantics.

The plan is revalidated at the public runtime boundary even after compilation. Strings, JSON, placeholder lookup, resource creation, and capacity growth remain outside measured steady-state frames. SDL/Vulkan ownership and the existing ScenarioSpec 0.1 API/hash behavior stay behind their original module boundaries.

Animation graphs and editor tooling are explicitly deferred. New gameplay behavior requires a reviewed built-in operation/reaction or a later separate scripting decision.

## Alternatives considered

- Keep v0.1 benchmark-only: rejected because it cannot test the intended human/AI-to-playable-game authoring loop.
- Add a general engine feature set and editor: rejected because no workload defines stable component reflection, serialization, inspector, undo, plugin, or asset-import contracts.
- Add arbitrary gameplay C++ or scripting first: rejected because it bypasses the compiler/validator hypothesis and reintroduces per-game code as the primary interface.
- Embed collision/audio/UI in Breakout-specific runtime code: rejected because it would not prove reusable declarative semantics.

## Consequences and evidence

The runtime gains concrete code and capacity limits, but no generic ECS, RHI, physics interface, event bus, plugin system, or editor abstraction. Unsupported behavior is rejected during compilation or public-plan validation. Scene loads and explicit resets remain structural allocation phases; steady-state UI formatting and execution remain allocation-measured.

Evidence is recorded in `docs/reports/v0.2-implementation-report.md`: declarative Breakout, focused collision/input/settings/scene/UI/asset tests, fresh Debug and Release builds, ASan, GPU-off, Vulkan standard and synchronization validation, presented fault injection, architecture checks, and the completed GPT Pro review/remediation.

## Revisit triggers

Revisit this decision only when a concrete declarative game requires one of the following and the current closed representation cannot express it without duplicated or unsafe metadata:

- a bounded animation clip/state operation;
- collision shapes or responses beyond the proven AABB gameplay contract;
- localization/shaping beyond static atlas glyphs;
- asset streaming or hot reload with a measured lifetime contract;
- enough repeated authoring friction to justify editor tooling with an already-stable schema.

Do not treat a feature wishlist alone as a trigger.
