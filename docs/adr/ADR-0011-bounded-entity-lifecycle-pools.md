# ADR-0011: Bounded entity lifecycle through object pools

## Status

Accepted for v0.4.

## Context

The v0.3 runtime can activate pre-authored prefixes, but it cannot repeatedly acquire, return, expire, and reuse individual objects without hard-coding slot indices into game rules. Projectiles, enemies, pickups, effects, and traps need a reusable lifecycle primitive. Runtime entity creation would make capacity, allocation, determinism, snapshots, and diagnostics harder to bound; a game-specific C++ spawn API would violate the declarative AI-first boundary.

## Decision

GameManifest/SceneSpec 0.4 adds named pools that exclusively own the active state of one pre-created spawn group. Each pool uses an ascending-index FIFO for available slots and an intrusive acquisition-order list for active slots. `skip` reports exhaustion without mutation; `recycle_oldest` removes and immediately reuses the earliest acquired active slot. All storage is allocated while loading a scene.

The ordered action set gains `spawn_from_pool`, `release_to_pool`, and `reset_pool`. Spawn restores authored components, resolves a bounded position source, applies optional velocity/rotation overrides, synchronizes interpolation history, activates the slot, and optionally registers an absolute expiration tick. Release deactivates a currently acquired slot and appends it to the FIFO. Reset restores the authored initial prefix and lifecycle order. Existing rule order provides scheduling; no spawn event, scheduler, expression language, or gameplay callback is added.

Pools are the sole active-state owner of their group. The compiler and public-plan validator reject direct activation/count/reset actions, legacy deactivate/reset reactions, transform-chain ownership, ambiguous collision endpoints, and component or event-context mismatches. Runtime invariant failures use `RUNTIME_POOL_STATE_INVALID`; authoring failures use `GAME_POOL_INVALID`.

Schema 0.2 and 0.3 parsing and hashing remain unchanged. Pool data and pool-action fields participate only in the 0.4 hash.

## Alternatives

- Runtime entity creation: rejected because it weakens fixed capacities and zero-growth evidence.
- Generic spawn/despawn events or a scheduler: rejected because fixed intervals, input, scene entry, and collision already provide bounded triggers.
- A general scripting API: rejected because it would bypass the closed typed action set.
- Prefix-only activation: rejected because it cannot express FIFO reuse, independent lifetimes, or oldest recycling.

## Consequences

The maximum number of objects remains known at scene load, and lifecycle operations are deterministic and O(1), apart from the explicitly linear active-slot lifetime scan. Authored scenes carry more slot metadata and must choose a capacity up front. A pool cannot participate in transform-chain ownership or legacy active-state reactions.

## Evidence and revisit trigger

Projectile Arena and Timed Pickups must exercise both exhaustion policies without game-specific C++. A 10,000-slot recycle test and sustained churn must report zero tracked steady-state allocations and capacity growth. Revisit only if multiple materially different 2D games need dynamic cardinality that cannot be bounded acceptably at scene load.
