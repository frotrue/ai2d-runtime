# Migrating GameManifest/SceneSpec 0.5 to 0.6

Migration is opt-in. Existing 0.2 through 0.5 content remains valid and retains its source and plan hashes.

1. Change the manifest and every referenced scene from `schema_version: "0.5"` to `"0.6"`.
2. Add manifest arrays `animations`, `prefabs`, and `localizations`; each may be empty.
3. Add at least one `input_profiles` entry and `default_input_profile`. An empty profile `bindings` array inherits the canonical action bindings.
4. Add scene arrays `tile_layers`, `fields`, `particle_emitters`, and `ui_stacks`; each may be empty.
5. Change the scene camera to the 0.6 form by adding `mode: "fixed"`, or use `mode: "follow"` with one fixed `{group,index}` target.
6. Keep every 0.5 collision `interaction`, contact capacity, pool, and linear-motion declaration unchanged unless the game actually adopts a new 0.6 primitive.
7. Add `persistent: true` only to scenes whose exact runtime snapshot is allowed in a save. If saves are needed, add the manifest `save` declaration with bounded slots and an explicit state allowlist.
8. Replace repeated component declarations with external `prefab-v1` files only when compile-time reuse helps. Do not model runtime inheritance or child graphs with prefabs.
9. Add animation clips and initial animation declarations only to sprite-bearing groups/prefabs. Use typed actions/events instead of a frame-script callback.
10. Add tile layers/fields only on existing logical grids and stay within the per-grid and aggregate cell limits.
11. Add localized `text_key` UI only after supplying identical key sets for every locale and a font containing every required glyph.
12. Add music as `kind: "music"` for streamed WAV. Keep short effects as `kind: "wav"`.
13. Extend or add a `game-test-v1` script for the new animation, cell, camera, and metric observations.

Minimal compatibility additions:

```json
"animations":[],
"prefabs":[],
"localizations":[],
"input_profiles":[{"id":"default","bindings":[]}],
"default_input_profile":"default"
```

```json
"persistent":false,
"camera":{"mode":"fixed","position":[0,0],"half_extent":[16,9]},
"tile_layers":[],
"fields":[],
"particle_emitters":[],
"ui_stacks":[]
```

Validate the migrated content and inspect its resolved dependencies and numeric plan:

```powershell
python tools/engine.py validate path/to/game.json --preset dev --json
build/dev/bin/ai2d_cli.exe game inspect path/to/game.json --json
python tools/engine.py verify path/to/game.json --test-script path/to/test.json --repeat 2 --preset dev --json
```

A migrated 0.6 document receives a new source/plan hash by design. Its plan hash uses normalized content-root-relative dependency names and therefore remains stable when the complete unchanged content tree is relocated. Unmodified 0.2–0.5 documents do not change. Save files are tied to the exact 0.6 plan hash and are intentionally not migrated across content changes. Choose `organization` and `application` values that are safe single path components; traversal, separators, trailing dots, and Windows device names are rejected.
