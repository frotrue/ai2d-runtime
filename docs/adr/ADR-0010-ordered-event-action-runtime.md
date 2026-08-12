# ADR-0010: Ordered typed Event–Action rules for v0.3

## Status

Accepted for v0.3.

## Context

The 0.2 reaction and system set is sufficient for Breakout but cannot express a Snake tail that follows historical transforms, bounded growth, self-collision, or deterministic placement on an unoccupied logical cell. Adding Snake-specific native code would invalidate the AI-first declarative-runtime experiment. A generic scripting language would violate the bounded execution and steady-state allocation contract.

## Decision

Add versioned 0.3 manifests/scenes with ordered typed rules of the form `event + all-of conditions + ordered actions`. All authoring references resolve to numeric indices at compile/load time. Events are limited to scene entry, logical action press/release, fixed intervals, and named collision rules. Targets are limited to a spawn group, a fixed spawn index, or collision A/B. Actions are a closed set covering integer state, activation, velocity/direction, reset/audio, and deterministic free-cell relocation.

Add logical grids, fixed-interval `grid_motion`, and post-collision `follow_transform_chain`. Chain reserves continue receiving transforms while inactive so activation is visually and physically correct. Collision physics executes first; legacy external reactions and new collision rules then observe each event in declaration order. Later rules immediately observe earlier state changes.

Snapshots include scene tick, grid direction/phase, activation, and relocation invocation counters. Runtime storage and dispatch tables are reserved before the frame loop. GameManifest/SceneSpec 0.2 are still parsed by their strict original contract and retain the existing hash algorithm.

Official Windows packaging ships an EXE beside a `content/` tree. Shader SPIR-V is linked into the executable; referenced content remains external and inspectable. SDL and the MSVC runtime are static, while the Vulkan loader/driver remains a documented system prerequisite.

## Alternatives

- Snake-specific operations: smaller initially, but fail the generality goal and cannot prove reuse.
- Node/edge visual graph: more flexible, but increases validation and recursive-execution surface without adding needed behavior.
- Lua/bytecode VM: expressive, but makes bounded work, diagnostics, authoring safety, and no-allocation evidence substantially harder.
- Tag/component queries: rejected because fixed group/index/event targets cover the selected games with predictable cost.
- Single-EXE content: rejected because v0.3 prioritizes transparent dependency inspection and reproducible packaging.

## Consequences

The schema is more verbose than handwritten gameplay code, but it is deterministic, inspectable, and hard to misuse. Each new behavior still requires an explicitly reviewed typed primitive. The model is not a general game scripting language and intentionally cannot express arbitrary entity searches or recursive event workflows.

## Evidence and revisit trigger

Snake and Grid Collector must both complete with zero game-specific C++. Tracked steady-state allocations and capacity growth must remain zero in the new systems. Revisit only if a second materially different bounded 2D game requires duplicated native metadata across compiler/runtime layers or cannot be expressed without a game-specific primitive.
