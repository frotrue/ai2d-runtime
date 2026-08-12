# Migrating GameManifest 0.2 to 0.3

Migration is optional. Existing 0.2 manifests and scenes continue to compile and run without edits. A manifest and every scene it references must use the same version.

To opt in:

1. Change `schema_version` to `"0.3"` in the manifest and all referenced scenes.
2. Optionally add a manifest `seed` (an unsigned 64-bit integer); it defaults to zero.
3. Give every collision rule a unique `id`. A 0.3 collision rule may use an empty `reactions` array when it exists only to publish a collision event.
4. Add `active_count` to a spawn group when only the leading reserve should begin active. If omitted it equals `count`.
5. Add `grids` and `rules` arrays (empty arrays are valid).

The original `axis_control`, `set_velocity_on_press`, `simulate_collisions`, transitions, UI, and legacy collision reactions remain available. New 0.3 content can additionally use `grid_motion`, `follow_transform_chain`, and ordered Event–Action rules.

The compiler rejects unknown fields, mixed versions, invalid references/targets, limits above the documented capacities, and a chain system not ordered after its referenced grid motion and collision simulation. Run:

```powershell
python tools/engine.py validate path/to/game.json --json
python tools/engine.py inspect plan path/to/game.json --json
```

Inspect reports the actual schema version and exact relative dependencies needed for packaging.
