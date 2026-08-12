# GameManifest and SceneSpec 0.3 quickstart

The smallest v0.3 project is a manifest, one scene, and at least one startup PNG/font asset. All paths are relative to the manifest directory and may not escape it. See `schemas/game-manifest-v0.3.schema.json`, `schemas/scene-v0.3.schema.json`, and the two v0.3 sample games for the machine-readable contract.

## Coordinates and components

- World positions and sprite sizes are floating-point world units.
- Every authored scalar and every derived spawn/grid endpoint must remain finite and representable as a 32-bit float. Validation occurs before runtime narrowing.
- A sprite `uv` is `[x, y, width, height]`, normalized to the source texture.
- Ordinary velocity is world units per second. `grid_motion` derives the velocity needed to move exactly one declared cell during its step tick.
- A collider group used by a group-wide velocity system must give `Velocity2D` to every member. A reflected collision endpoint must contain only dynamic colliders with `Velocity2D`. Kinematic bodies are author-driven obstacles; static bodies do not integrate velocity.
- Deactivation suppresses rendering and collision while retaining transform/component storage.
- Font metadata lists explicit Unicode codepoints. Every literal UI glyph, decimal digit used by a placeholder, and minus sign required by a negative state range must exist in the font.

## Rules

Rules run in scene declaration order. Conditions are all-of. Every action is applied immediately, so a later rule in the same tick/event sees the new state. Collision solver `reflect`/`deactivate` reactions happen before external reactions and collision-event rules.

Supported events are `scene_enter`, `action_pressed`, `action_released`, `fixed_interval`, and named `collision`. Supported condition comparisons are `eq`, `ne`, `lt`, `le`, `gt`, and `ge` over integer state or an active spawn-group count.

Targets are `{kind:"group",group:"..."}`, `{kind:"index",group:"...",index:N}`, `{kind:"collision_a"}`, or `{kind:"collision_b"}`. Collision targets are valid only in collision-event rules.

`grid_motion` queues cardinal direction changes and commits the last valid direction on a step. Its required boolean `prevent_reverse` decides whether a direct 180-degree change is ignored. `follow_transform_chain` must appear after `simulate_collisions`, moves every reserve follower (active or inactive) to the predecessor's previous transform, and uniquely owns its follower spawn group.

Free-cell relocation marks active members of its `occupied_groups`, starts at a deterministic seed/rule/invocation-derived cell, and scans circularly. On a full grid it deactivates the target and optionally writes `0` to `result_state`; success writes `1`.

A scene may declare at most 64 logical grids, with at most 1,000,000 cells in each grid. A scene has at most 512 rules; each rule has at most eight conditions and sixteen actions. These and the schema's remaining limits are runtime memory contracts, not merely authoring recommendations.

PNG/font and WAV files have a 64 MiB source limit and a 256 MiB per-asset decoded limit. Official packaging and presented runtime startup preflight a 512 MiB aggregate decoded-resident budget. Images are decoded, uploaded, and released one at a time. WAV files are not decoded when audio is disabled or unavailable.

## Deterministic input

`game-input-v1` uses zero-based fixed ticks:

```json
{"schema_version":"1","events":[
  {"tick":0,"action":"right","kind":"press"},
  {"tick":12,"action":"right","kind":"release"},
  {"tick":12,"action":"down","kind":"tap"}
]}
```

Run it without real-time input:

```powershell
python tools/engine.py run samples/snake/game.json --headless --frames 180 --input-script samples/snake/input/grow-and-turn.json --json
```

## Windows package

```powershell
python tools/engine.py package samples/snake/game.json --output artifacts/packages --zip --json
```

The resulting portable folder/ZIP has `<application>.exe` and `content/game.json` plus exactly the inspected dependencies. It requires a Vulkan 1.3-capable system loader, driver, and GPU. Existing outputs are never overwritten without `--force`; forced directory/ZIP replacement uses same-filesystem backups and rolls the visible pair back if publication fails.
