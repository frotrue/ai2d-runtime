# Migrating GameManifest/SceneSpec 0.4 to 0.5

Migration is opt-in. Existing 0.2, 0.3, and 0.4 content remains valid and retains its source and plan hashes.

1. Change the manifest `schema_version` from `0.4` to `0.5`.
2. Change every referenced scene to `schema_version: "0.5"`.
3. Remove every authored `collider.trigger` field. In 0.5, interaction belongs to a named collision rule.
4. Add `interaction` to every collision rule:
   - use `trigger` with an empty `reactions` array for observational contacts;
   - use `solid` with exactly one dynamic endpoint, one static/kinematic endpoint, and at least one `reflect` reaction for physical reflection.
5. If any trigger rule exists, add `collision.max_contact_pairs`. Keep it between 1 and 100,000 and no greater than `max_candidate_pairs`.
6. Replace proxy/follower movement used only to mirror a moving contact object with a direct kinematic group and one `linear_motion` system where appropriate.
7. Replace typed `collision` events for observational rules with `contact_begin`; add `contact_end` only when separation matters.
8. Add a `game-test-v1` script and verify deterministic behavior with the official command.

Example trigger rule and motion system:

```json
{"id":"bullet_hits_enemy","a":"bullet","b":"enemy","interaction":"trigger","reactions":[]}
```

```json
{"id":"bullet_motion","operation":"linear_motion","spawn_group":"bullet_slots"}
```

```powershell
python tools/engine.py validate path/to/game.json --preset dev --json
build/dev/bin/ai2d_cli.exe game inspect path/to/game.json --json
python tools/engine.py verify path/to/game.json --test-script path/to/test.json --repeat 2 --preset dev --json
```

A migrated 0.5 document has a new source/plan hash by design. Unmodified 0.2–0.4 documents do not change.
