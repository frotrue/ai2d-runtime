# GameManifest/SceneSpec 0.4 quickstart and contract

GameSpec 0.4 extends 0.3 with bounded object pools. Read this together with `schemas/game-manifest-v0.4.schema.json` and `schemas/scene-v0.4.schema.json`. Paths remain relative to the manifest root and may not escape it or traverse links/junctions.

## Minimal files

The manifest is the 0.3 manifest shape with `"schema_version":"0.4"`. Every referenced scene must also use 0.4 and must include `pools`, even when empty:

```json
{
  "schema_version":"0.4",
  "id":"game",
  "world":{"capacity":16},
  "camera":{"position":[0,0],"half_extent":[16,9]},
  "grids":[],
  "spawn_groups":[],
  "pools":[],
  "systems":[],
  "rules":[]
}
```

The manifest and every referenced scene must have the same schema version. A scene may declare at most 1,024 pools, and all spawn groups together may contain at most the scene/world limit of 10,000 objects.

## Declaring a pool

First author every possible slot in a normal spawn group. `active_count` is the number of initially acquired prefix slots. These initial slots are active in index order and have no automatic lifetime.

```json
{
  "id":"projectile_slots",
  "count":32,
  "active_count":0,
  "placement":{"kind":"grid","origin":[0,0],"spacing":[0,0],"columns":1},
  "velocity":{"linear":[0,0]},
  "sprite":{"texture":"sprites","size":[0.5,0.5]},
  "collider":{"half_extent":[0.25,0.25],"group":"projectile","body":"dynamic","trigger":true}
}
```

Then name the lifecycle owner:

```json
{"id":"projectiles","group":"projectile_slots","on_exhausted":"recycle_oldest"}
```

One spawn group may belong to at most one pool. Available slots begin as an ascending-index FIFO. Returned slots append to its back.

- `skip`: no slot is acquired; optional `result_state` becomes `0`.
- `recycle_oldest`: the earliest acquired active slot is returned and immediately acquired again. Success and recycle set `result_state` to `1`.

## Spawning

`spawn_from_pool` is an ordered action. `position` is required and has exactly one of three forms:

```json
{"kind":"initial","offset":[0,0]}
```

Uses the selected slot's authored transform position.

```json
{"kind":"constant","value":[4,2],"offset":[0,0]}
```

Uses a fixed world position.

```json
{"kind":"target","target":{"kind":"index","group":"muzzle","index":0},"offset":[0.5,0]}
```

Copies one fixed spawn index. In a collision rule, `collision_a` or `collision_b` is also permitted. A whole-group target is never permitted.

Complete example:

```json
{
  "kind":"spawn_from_pool",
  "pool":"projectiles",
  "position":{"kind":"target","target":{"kind":"index","group":"muzzle","index":0}},
  "velocity":[18,0],
  "rotation":0,
  "lifetime_ticks":180,
  "result_state":"spawn_succeeded"
}
```

Before activation the runtime restores authored Transform2D, Velocity2D, Sprite2D, and Collider2D values. It then resolves position in double precision, applies optional velocity and rotation, assigns `previous_position = position`, activates the slot, and installs the optional lifetime. A velocity override requires every slot in the pooled group to have Velocity2D. `lifetime_ticks` is 1 through 1,000,000.

An object acquired during tick `T` with lifetime `N` is returned before processing tick boundary `T+N`. A collision-created object is still rendered at least once before it can expire.

## Releasing and resetting

Release a fixed pooled index or the current collision endpoint:

```json
{"kind":"release_to_pool","pool":"projectiles","target":{"kind":"collision_a"},"result_state":"release_succeeded"}
```

An acquired slot is deactivated, loses its lifetime, appends to the available FIFO, and writes `1`. A slot already available is unchanged and writes `0`. Its current transform remains until the next acquisition restores authored components. The compiler rejects collision endpoints that could resolve to anything outside the named pool.

Reset all components and lifecycle order to the authored initial snapshot:

```json
{"kind":"reset_pool","pool":"projectiles"}
```

Actions remain strictly ordered. If an exhausted one-slot pool is released and then spawned later in the same rule, that returned slot is immediately available.

## Events, conditions, and ownership

Pools add no event or scheduler. Use existing `scene_enter`, `fixed_interval`, logical input, and named collision events. `group_active_count` may read a pooled group. Existing velocity, collision, rendering, grid motion, and relocation occupancy are allowed.

The following are rejected for a pooled group because the pool is its sole active-state owner:

- `activate`, `deactivate`, `set_group_active_count`, or `reset_group`;
- legacy collision `deactivate` or `reset_group` reactions;
- use as a `follow_transform_chain` leader or follower;
- a `release_to_pool` target not provably owned by the named pool.

## Snapshot and determinism

Scene snapshots include slot acquired flags, available FIFO order, active acquisition order, and every expiration tick. `reset_scene` and `reset_pool` restore authored initial state. Retained scene re-entry restores the exact saved lifecycle state. Pool declarations and pool action fields affect only 0.4 plan hashes; 0.2 and 0.3 hash algorithms are unchanged.

## Diagnostics and metrics

Invalid authored pools/actions use `GAME_POOL_INVALID`. A corrupted internal FIFO/list/slot invariant uses `RUNTIME_POOL_STATE_INVALID`.

`game run --json` reports:

- `pool_acquire_attempts`, `pool_acquire_successes`;
- `pool_releases`, `pool_release_misses`;
- `pool_exhaustions`, `pool_recycled_slots`, `pool_expirations`, `pool_resets`;
- `pool_lifetime_checks`;
- `active_pooled_entities`, `peak_active_pooled_entities`.

`game inspect --json` lists pool ID, group, capacity, initial active count, exhaustion policy, and compiled pool actions.

## Input scripts, run, and package

Input scripts still use `game-input-v1` (`schema_version: "1"`) and 0-based ticks:

```json
{"schema_version":"1","events":[{"tick":0,"action":"fire","kind":"tap"}]}
```

```powershell
build/dev/bin/ai2d_cli.exe game validate path/to/game.json --json
build/dev/bin/ai2d_cli.exe game inspect path/to/game.json --json
build/dev/bin/ai2d_cli.exe game run path/to/game.json --headless --frames 600 --input-script path/to/input.json --no-audio --no-saved-settings --json
python tools/engine.py package path/to/game.json --output artifacts/packages --zip --json
```

The Windows package remains `<application>-windows-x64/` plus a deterministic ZIP. The EXE automatically loads `content/game.json`; Vulkan loader/driver support remains a system requirement.

## Scope boundary

0.4 does not add sprite animation, save/offline progression, tilemaps, particles, UI expansion, state machines, scripting/VM, runtime entity creation, or a dedicated spawn scheduler.
