# ADR-0013: Bounded content foundations instead of general engine subsystems

## Context

Versions 0.2–0.5 proved a closed declarative gameplay path, deterministic pools, moving contacts, black-box verification, and Windows packaging. Black-box games can express non-trivial rules, but production breadth is now limited by static sprites, repeated scene JSON, process-local state, non-rendered logical grids, fixed input/UI/camera, resident-only effects audio, and no presentation effect pool.

General-purpose engines solve these gaps with mutable object graphs, scripting, import databases, runtime factories, animation graphs, editor widget trees, and broad serialization. Those mechanisms conflict with this project's numeric plans, bounded memory, fixed components, measurable hot paths, and AI-observable diagnostics.

## Decision

Version 0.6 adds only the bounded contracts recorded in `docs/v0.6-plan.md`:

- tick-authored sprite clips over the existing sprite component;
- non-recursive compile-time prefab expansion;
- plan-bound binary save slots with complete validation before mutation;
- fixed logical-grid-backed tile and integer-field arrays;
- compiled input profiles, deterministic camera rigs, compile-time UI anchors/stacks and localization tables, one bounded PCM WAV music stream, and CPU particle slots reserved at scene load.

Every runtime name is resolved before execution. Fixed-tick storage is reserved during structural phases. File I/O is queued out of the fixed tick. The implementation extends concrete plans and operations; it does not introduce registries, reflection, arbitrary expressions, callbacks, or runtime entity creation.

## Alternatives considered

- **Adopt a scripting VM or gameplay C++ plug-in.** Rejected because it bypasses the compiler's inspectable and bounded contract.
- **Use a generic ECS/serialization framework.** Rejected because the required data is known and concrete, and framework reflection would weaken dependency and memory guarantees.
- **Build a visual editor first.** Rejected because the immediate bottleneck is runtime expressiveness and reusable data, while AI authoring benefits more from schemas, diagnostics, inspect, and verify.
- **Implement only an Oxygen Not Included-style field simulation.** Rejected as genre-specific. Tile and integer-field primitives are reusable and intentionally do not prescribe gas, heat, plumbing, or colony rules.
- **Add all common-engine presentation features.** Rejected. The accepted camera, layout, music, and particle slices are the smallest deterministic contracts that can prove the commercial baseline.

## Consequences

The 0.6 schema is larger and compiler validation becomes more involved. Save compatibility is deliberately tied to the exact plan hash. Music is limited to streamable WAV and particle effects remain CPU/simple-sprite based. In return, authored games gain visible animation, reusable definitions, durable progress, large cell worlds, controller-ready input, responsive authored UI placement, localized text, camera feedback, music, and effects without opening an arbitrary extension path.

## Revisit triggers

- authored content cannot share components without recursive composition;
- save migration across plan changes becomes a measured product requirement;
- four million scene cells or linear extraction becomes a demonstrated bottleneck;
- one stream or CPU particles fail a real sample's measured presentation need;
- compile-time layout cannot express a source-blind commercial menu without duplicated coordinates.
