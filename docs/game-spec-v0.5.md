# GameManifest/SceneSpec 0.5 quickstart and contract

GameSpec 0.5 extends 0.4 with bounded motion contacts and deterministic black-box verification. Read this contract together with `schemas/game-manifest-v0.5.schema.json`, `schemas/scene-v0.5.schema.json`, `schemas/game-test-v1.schema.json`, and `schemas/font-metadata-v1.schema.json`. Manifest-relative paths may not escape the content root or traverse links/junctions.

## Minimal scene

The manifest and every referenced scene must use `schema_version: "0.5"`. A scene still requires the 0.4 `pools` array, even when empty.

```json
{
  "schema_version":"0.5",
  "id":"play",
  "world":{"capacity":16},
  "camera":{"position":[0,0],"half_extent":[16,9]},
  "collision":{"bounds":[-18,-10,18,10],"cell_size":[2,2],"max_colliders":16,"max_grid_references":128,"max_candidate_pairs":64,"max_contact_pairs":32,"max_impacts":4},
  "grids":[],"spawn_groups":[],"pools":[],"systems":[],"collision_rules":[],"rules":[]
}
```

## Body and interaction compatibility

Body motion describes who changes a transform; interaction describes what a matching rule does.

| Interaction | Endpoint A/B combinations | Physical response | Typed event | Reactions |
|---|---|---|---|---|
| `trigger` | static, kinematic, or dynamic in any pairing | none | `contact_begin`, `contact_end` | must be `[]` |
| `solid` | exactly one dynamic; other static or kinematic | swept-AABB reflection | `collision` | at least one `reflect` |

`static` objects may not be moved by an authored motion system. `kinematic` objects are moved only by authored systems and receive no solid response. `dynamic` objects are integrated and reflected by `simulate_collisions`. The old collider-level `trigger` field is invalid in 0.5; it remains unchanged in schemas 0.2–0.4.

Every 0.5 collision rule declares interaction:

```json
{"id":"bullet_hits_enemy","a":"bullet","b":"enemy","interaction":"trigger","reactions":[]}
```

A scene containing a trigger rule must declare `collision.max_contact_pairs` from 1 through 100,000. It may not exceed `max_candidate_pairs`; the aggregate across all scenes may not exceed 1,000,000. Candidate, contact, event, and motion-segment capacity arithmetic is overflow checked.

## Linear motion and units

Velocity is world units per second. At the fixed 60 Hz tick, `linear_motion` performs `position += velocity * (1/60)` for each active slot:

```json
{"id":"bullet_motion","operation":"linear_motion","spawn_group":"bullet_slots"}
```

Every target slot needs Velocity2D. A target collider must be kinematic; dynamic colliders are rejected to prevent double integration. A group may have only one linear-motion owner and may not overlap `axis_control` or follower ownership. Pooled groups are allowed.

Authored system order is significant. A kinematic `grid_motion` that also feeds linear motion must precede `linear_motion`, which must precede `simulate_collisions`. Existing dynamic grid motion keeps its 0.3 behavior.

## Contact lifecycle

The runtime records static, kinematic (`previous_position -> position`), and reflected dynamic motion segments. Relative swept AABB detects high-speed contacts between any allowed trigger bodies.

- A pair absent on the previous tick that sweeps or ends overlapped emits one `contact_begin`.
- A persistently overlapping pair does not repeat begin.
- A retained pair whose active endpoints separate emits one `contact_end`.
- A pair that completely passes through in one tick emits begin only and is not retained.
- Release, recycle, reset, or deactivation removes identity immediately and emits no end.
- A recycled slot is a new lifecycle identity even though its EntityId is unchanged.

Solid reactions run first. Collision impacts and trigger begins then run in `(TOI, collision-rule declaration order, A ID, B ID)` order. Ends run in `(collision-rule declaration order, A ID, B ID)` order. Typed rules for one event run in scene declaration order. If an earlier event releases or deactivates an endpoint, later stale events for that lifecycle are skipped.

Contact events capture TOI, normal, and each endpoint position at contact. In a 0.5 collision/contact rule, a `collision_a` or `collision_b` position source uses that captured contact position rather than the live end-of-tick transform. This allows direct contact-position item/effect spawning.

Active pairs and their numeric rule/entity identities are part of retained snapshots. `reset_scene` starts with an empty contact set; retained re-entry restores the exact set.

## Deterministic black-box tests

`game-test-v1` uses 0-based ticks. Assertions are evaluated immediately after their tick executes. Events and assertions must be nondecreasing by tick.

```json
{
  "schema_version":"1",
  "frames":120,
  "events":[{"tick":0,"action":"fire","kind":"tap"}],
  "assertions":[
    {"tick":0,"kind":"position","scene":"play","group":"bullet_slots","index":0,"expected":[-9.8,0],"tolerance":0.0001},
    {"tick":60,"kind":"int_state","state":"score","op":"ge","value":1},
    {"tick":119,"kind":"runtime_metric","metric":"contact_begins","op":"ge","value":1}
  ]
}
```

Supported assertions are `current_scene`, `int_state`, `group_active_count`, `entity_active`, `position`, `velocity`, and `runtime_metric`. Integer/metric comparisons use `eq/ne/lt/le/gt/ge`. Position/velocity target one scene, group, and fixed index; tolerance defaults to `1e-5` and is bounded to 0–1. All names compile to numeric indices before tick execution.

```powershell
python tools/engine.py verify path/to/game.json --test-script path/to/test.json --repeat 2 --preset dev --json
```

`repeat` is 1–16 and defaults to 2. Each repetition constructs a fresh runtime. The command compares assertion observations, final scene/state, state checksum, contact-state checksum, and cumulative metrics. Assertion mismatches use `GAME_TEST_ASSERTION_FAILED`; repetition divergence uses `GAME_TEST_NONDETERMINISTIC`.

The initial scene's `scene_enter` rules execute during `GameRuntime::initialize` and are not charged to the first measured tick. A later transition or retained re-entry executes `scene_enter` inside the transition frame, so its rule, condition, action, pool, and active-state effects are included in that frame's cumulative metrics. Contact and linear-motion metrics cover only fixed ticks executed by the verification run.

## Metrics and diagnostics

Run/verify JSON adds `trigger_narrowphase_tests`, `contact_begins`, `contact_ends`, `stale_contact_events`, `active_contact_pairs`, `peak_contact_pairs`, `motion_segments`, and `linear_motion_updates`. Inspect reports interaction, event phase, contact capacity, and compiled motion ownership.

Invalid authored interaction/capacity uses `GAME_COLLISION_INTERACTION_INVALID`; corrupted active-pair/snapshot state uses `RUNTIME_CONTACT_STATE_INVALID`; exhausted fixed contact/event capacity uses `COLLISION_CONTACT_CAPACITY_EXCEEDED`.

## Font metadata v1

A font asset names its PNG atlas and metadata JSON. Metadata has exactly `schema_version`, positive finite `line_height`, and 1–65,536 glyph objects. Each glyph contains Unicode scalar `codepoint` (1–1,114,111), normalized `[u,v,width,height]` `uv`, finite `size` and `bearing` vectors, and nonnegative finite `advance`. UV components are each 0–1. Static UI text and state placeholders must be representable by the supplied glyph set.

## Commands and scope

```powershell
python tools/engine.py validate samples/pool_siege/game.json --preset dev --json
build/dev/bin/ai2d_cli.exe game inspect samples/pool_siege/game.json --json
python tools/engine.py verify samples/pool_siege/game.json --test-script samples/pool_siege/tests/smoke.json --repeat 2 --preset dev --json
python tools/engine.py package samples/pool_siege/game.json --output artifacts/packages --zip --json
```

v0.5 does not add dynamic-dynamic solid resolution, mass/force/friction, rotating collisions, joints, frame capture/golden images, arbitrary expressions, scripting/VM, callbacks, runtime entity creation, or a generic ECS.
