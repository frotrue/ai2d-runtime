# Migrating GameManifest/SceneSpec 0.3 to 0.4

Migration is opt-in. Existing 0.3 content remains valid and retains its source and plan hashes.

1. Change the manifest `schema_version` from `0.3` to `0.4`.
2. Change every referenced scene to `schema_version: "0.4"`.
3. Add a required `pools` array to every scene. Use `"pools": []` when the scene has no pooled objects.
4. For each repeatable object type, author the maximum slot count as a spawn group and set `active_count` to the initial acquired prefix.
5. Add one pool declaration naming that group and an explicit `skip` or `recycle_oldest` policy.
6. Replace direct active-state manipulation of that group with `spawn_from_pool`, `release_to_pool`, and `reset_pool`.

Pooled groups may still be rendered, collided, moved by existing velocity/grid systems, counted by `group_active_count`, and used as relocation occupancy. They may not be activated/deactivated/reset directly, targeted by legacy deactivate/reset reactions, or used as transform-chain leaders/followers.

Validate and inspect the migrated game:

```powershell
build/dev/bin/ai2d_cli.exe game validate path/to/game.json --json
build/dev/bin/ai2d_cli.exe game inspect path/to/game.json --json
```

Because a 0.4 document has a new schema version and new hash input, its hash is expected to differ from the former 0.3 document. Unmodified 0.2/0.3 documents do not change.
