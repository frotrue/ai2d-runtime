# GameManifest/SceneSpec 0.6 quickstart and contract

GameSpec 0.6 extends 0.5 with bounded content and presentation foundations. Read this contract with `schemas/game-manifest-v0.6.schema.json`, `schemas/scene-v0.6.schema.json`, `schemas/prefab-v1.schema.json`, `schemas/localization-v1.schema.json`, `schemas/game-test-v1.schema.json`, and `schemas/font-metadata-v1.schema.json`. Manifest-relative dependencies must remain below the content root and may not traverse symlinks or Windows reparse points. The 0.6 plan identity hashes normalized content-root-relative dependency names, so moving an unchanged content tree does not invalidate its saves. `organization` and `application` are safe single path components and may not be Windows device names.

## Minimal 0.6 game

The manifest and every referenced scene must use `schema_version: "0.6"`. Older versions remain separate contracts; versions may not be mixed.

```json
{
  "schema_version":"0.6",
  "seed":1,
  "name":"Minimal 0.6",
  "organization":"Example",
  "application":"minimal_v06",
  "window":{"title":"Minimal","width":1280,"height":720,"virtual_width":1280,"virtual_height":720,"default_fps":"60"},
  "assets":[{"id":"sprites","kind":"png","path":"assets/sprites.png"}],
  "animations":[],
  "prefabs":[],
  "localizations":[],
  "actions":[{"id":"quit","keys":["escape"]}],
  "input_profiles":[{"id":"default","bindings":[]}],
  "default_input_profile":"default",
  "states":[],
  "scenes":[{"id":"play","path":"scenes/play.json"}],
  "start_scene":"play",
  "transitions":[],
  "fps_actions":[]
}
```

```json
{
  "schema_version":"0.6",
  "id":"play",
  "persistent":false,
  "world":{"capacity":16},
  "camera":{"mode":"fixed","position":[0,0],"half_extent":[16,9]},
  "grids":[],"tile_layers":[],"fields":[],"particle_emitters":[],
  "spawn_groups":[],"pools":[],"systems":[],"collision_rules":[],"rules":[],
  "ui":[],"ui_stacks":[]
}
```

## Sprite animation

Animation timing uses fixed ticks, never wall-clock milliseconds. A clip has 1–256 normalized UV frames and one playback mode:

```json
{"id":"walk","texture":"sprites","mode":"loop","frames":[
  {"uv":[0,0,0.25,1],"duration_ticks":4},
  {"uv":[0.25,0,0.25,1],"duration_ticks":4}
]}
```

Modes are `once`, `loop`, and `ping_pong`. Frame duration is 1–1,000,000 ticks. A sprite-bearing prefab or spawn group may declare `animation` and `autoplay`. Ordered `play_animation` and `stop_animation` actions support group, fixed-index, collision A/B, and `event_entity` targets. A `once` clip emits one `animation_finished` event; matching rules execute in scene declaration order, and `event_entity` identifies the completed entity.

Animation state is part of scene snapshots and saves. Pool acquire/reset restores authored initial animation state. Animation never changes collision geometry.

## Compile-time prefabs

Manifest `prefabs` entries name external `prefab-v1` files. A prefab may contain at most one transform, velocity, sprite, collider, and initial animation declaration. It may not contain a child, group, pool, rule, system, script, or prefab reference.

```json
{
  "schema_version":"1",
  "transform":{"position_offset":[0,0],"rotation":0,"scale":[1,1]},
  "velocity":{"linear":[0,0],"angular":0},
  "sprite":{"texture":"sprites","size":[1,1],"uv":[0,0,0.25,1],"layer":10},
  "animation":{"clip":"walk","autoplay":true}
}
```

A spawn group references a prefab and may replace individual components as complete authored values. Expansion occurs in the compiler. The numeric `GamePlan` and runtime contain only resolved fixed components; there is no runtime inheritance, factory, reflection, or recursive composition.

## Tile layers and integer fields

Tile and field storage is backed by a declared logical grid. Each grid has at most 1,000,000 cells. A scene has at most 64 tile layers and 64 fields. Tile-plus-field cells are capped at 4,000,000 both per scene and across the complete game because retained/save snapshot storage is reserved for every scene.

```json
"grids":[{"id":"board","first_cell_center":[-3.5,-1.5],"cell_size":[1,1],"columns":8,"rows":4}],
"tile_layers":[{
  "id":"floor","grid":"board","texture":"tiles","atlas_columns":4,"atlas_rows":2,
  "fill":1,"tint":[1,1,1,1],"layer":-20,"visible":true
}],
"fields":[{"id":"heat","grid":"board","min":0,"max":100,"initial":0}]
```

Tile ID 0 is empty; positive IDs select the row-major atlas cell. `set_tile`, `set_field`, and `add_field` mutate one constant or target-derived cell. `add_field` clamps to the authored range. `tile_value` and `field_value` conditions read cells. A target outside the grid performs no write and sets an optional result state to 0; a valid access sets it to 1. All storage and observation targets are numeric before the first tick.

This is not a chunk streamer, tile collision baker, terrain editor, pathfinder, fluid simulator, or general cellular-automata scheduler.

## Versioned save slots

Save configuration is optional and names the integer states that are durable:

```json
"save":{"slots":2,"states":["score","unlocked_stage"]}
```

Slots are 0-based constants in `save_slot`, `load_slot`, and `delete_slot` actions. A result state receives 1 for success and 0 for failure. Only one request may be pending in a tick; a later concurrent request is rejected deterministically and increments the failure metric.

Save I/O does not execute inside `fixed_tick`. A request is handled immediately after the tick that emitted it, including when several exact ticks are batched in one API call. Save observations therefore do not depend on caller batching.

The binary format stores an 8-byte 0.6 magic, format version, exact plan hash, payload length, and payload checksum. It contains the selected integer states, active persistent scene, global simulation tick, retained persistent scene snapshots, locale, and input profile. Snapshots include each scene tick, fixed components, lifecycle pools, contacts, animation, tile/field arrays, camera, particles, rule/system phases, and deterministic counters. Only scenes authored with `persistent: true` can be saved.

Writes use a unique per-writer sibling temporary file plus atomic replacement; concurrent writers never share temporary storage and the last completed replacement wins. Loads parse and validate every bounded counter and derived invariant into reserved scratch, then commit transactionally. Bad magic, length, checksum, indices, values, snapshot invariants, or plan hash produce `RUNTIME_SAVE_STATE_INVALID` without partial mutation. The exact encoded upper bound is computed by the compiler and public-plan validator, reserved at initialization, and capped at 64 MiB. This version deliberately provides no cross-plan migration.

## Input profiles and gamepads

The manifest still declares the canonical logical action list. Each 0.6 input profile contains zero or more overrides; an empty override list inherits the canonical bindings.

```json
"input_profiles":[
  {"id":"keyboard","bindings":[]},
  {"id":"gamepad","bindings":[
    {"action":"move_left","keys":[],"gamepad_axes":[{"axis":"left_x","direction":"negative","deadzone":0.25}]},
    {"action":"move_right","keys":[],"gamepad_axes":[{"axis":"left_x","direction":"positive","deadzone":0.25}]},
    {"action":"fire","keys":[],"gamepad_buttons":["south"]}
  ]}
],
"default_input_profile":"keyboard"
```

One action supports at most four keys, four buttons, and two axes. Deadzone is finite and in `(0,0.95]`. The platform polls one active SDL gamepad. Logical press/release edges are transitions of the aggregate binding state, so pressing a second constituent while the action is already down does not retrigger it. Disconnects and profile changes release actions that were held by removed bindings; a profile change inside a batched run is re-resolved at the next fixed-tick boundary. `set_input_profile` names a compile-time profile constant. The selected profile is included in a versioned save.

## Camera

A fixed camera uses authored `position` and `half_extent`. A follow camera names exactly one fixed spawn index:

```json
"camera":{
  "mode":"follow","position":[0,0],"half_extent":[8,4.5],
  "target":{"group":"player","index":0},"offset":[0,0],
  "bounds":[-20,-10,40,20],"pixel_snap":true
}
```

Bounds constrain the camera center. Follow updates after gameplay systems and contacts. Pixel snapping is deterministic. `camera_shake` uses bounded amplitude/duration and seed-derived offsets. `set_camera_zoom` accepts `[0.1,10]`. Camera state participates in persistent snapshots and save data.

## Anchored UI and localization

UI elements support `top_left`, `top`, `top_right`, `left`, `center`, `right`, `bottom_left`, `bottom`, and `bottom_right` anchors. Authored and compiled positions are element centers. A stack owns an ordered list of UI child IDs and resolves horizontal or vertical child centers at scene compilation using its centered rectangle, padding, and spacing; public-plan validation repeats the complete rectangle check. A child may belong to only one stack. This is compile-time layout, not a runtime widget graph.

Localized text uses `text_key` instead of `text`. The manifest maps locale IDs to `localization-v1` files:

```json
{"schema_version":"1","strings":{"title":"CONTENT FOUNDATIONS","help":"ARROWS MOVE"}}
```

All locale files must contain identical ordered keys. Values are checked as UTF-8 against the chosen font glyphs and placeholder rules. Renderer capacity uses the largest one active locale, not the sum of every locale. `set_locale` names a compiled locale constant; the selected locale is saved.

## Streamed music

Music assets use `kind: "music"` and a WAV path. One stream can play at a time. `play_music` declares loop, volume, and fade ticks; `stop_music` and `set_music_volume` optionally fade. The runtime reads bounded blocks outside `fixed_tick` into a preallocated 32,768-frame stereo-float ring mixed by the existing audio callback. Music is not counted as resident decoded startup audio. Unsupported/corrupt WAV data reports a structured asset/runtime diagnostic; disabling audio skips stream decoding.

This slice does not provide compressed codecs, playlists, seeking, DSP nodes, spatial music, or a general audio graph.

## Preallocated particles

A scene has at most 64 emitters. Each emitter has 1–10,000 slots. Particle capacity is capped at 100,000 both per scene and across the complete game because snapshot slots are reserved up front:

```json
{"id":"sparks","capacity":64,"on_exhausted":"recycle_oldest",
 "sprite":{"texture":"sprites","size":[0.2,0.2],"uv":[0.75,0,0.25,1],"layer":30},
 "lifetime_ticks":[8,16],"velocity_min":[-2,1],"velocity_max":[2,4]}
```

`emit_particles` emits a constant bounded count at a constant or target-derived position. Velocity/lifetime selection is seed-derived and deterministic. Exhaustion is `skip` or `recycle_oldest`. A preallocated minimum heap selects the lowest available slot and an intrusive active queue provides constant-time oldest recycling; slot work is exposed as `particle_slot_operations`. Slots update and expire without allocation or a capacity-wide search per emission. Particles render as sprites but do not collide, execute rules, or become entity targets.

## Black-box verification additions

`game-test-v1` retains its 0-based tick semantics and adds `animation_frame`, `tile_value`, `field_value`, and `camera_position` assertions. Runtime metrics include the new animation, tile/field, save, particle, camera, music, profile, and locale counters. Names compile before execution; repeated runs compare observations, state, checksums, and metrics. When saves are enabled, `game verify` creates a private temporary save root and a clean subdirectory for every repeat, so save/load assertions cannot observe user data or a previous repeat.

```powershell
python tools/engine.py validate samples/content_foundations/game.json --preset dev --json
build/dev/bin/ai2d_cli.exe game inspect samples/content_foundations/game.json --json
python tools/engine.py verify samples/content_foundations/game.json --test-script samples/content_foundations/input/verify.json --repeat 2 --preset dev --json
python tools/engine.py package samples/content_foundations/game.json --output artifacts/packages --zip --json
```

## Diagnostics and exclusions

Authoring diagnostics are `GAME_ANIMATION_INVALID`, `GAME_PREFAB_INVALID`, `GAME_SAVE_INVALID`, `GAME_TILE_FIELD_INVALID`, `GAME_INPUT_PROFILE_INVALID`, `GAME_LOCALIZATION_INVALID`, and `GAME_PRESENTATION_INVALID`. Corrupt runtime state uses `RUNTIME_ANIMATION_STATE_INVALID`, `RUNTIME_SAVE_STATE_INVALID`, or `RUNTIME_TILE_FIELD_STATE_INVALID`.

Version 0.6 does not add scripting/VM, arbitrary callbacks or expressions, runtime entity creation, recursive prefabs, save migration, animation graphs, general UI layout, compressed music codecs, generic asset import, tile streaming, fluid simulation, particle collision, lighting, editor, or hot reload.
