# ScenarioSpec v0.1

ScenarioSpec is a deliberately small load-time format for deterministic sprite workloads. It is not a scripting language, generic game IR, or extension API. The machine-readable companion is [`schemas/scenario-v0.1.schema.json`](../schemas/scenario-v0.1.schema.json); the compiler also performs the semantic checks that JSON Schema cannot express.

## Required root fields

Every document contains:

- `schema_version`: exactly `"0.1"`
- `name`: non-empty scenario name
- `seed`: non-negative deterministic seed
- `world.capacity`: reserved entity/component capacity, up to 1,000,000
- `camera.position` and positive `camera.half_extent`
- `textures`: up to 64 procedural `solid` or `checker` declarations
- `spawn_groups`: up to 256 deterministic grid or seeded-random groups
- `systems`: up to 64 built-in system declarations
- `benchmark`: `runs`, `warmup_frames`, and `measurement_frames`

Vectors are JSON arrays such as `[x,y]`; colors are linear RGBA arrays in `[0,1]`. A spawn group always has `id`, positive `count`, `placement`, and `transform`. `velocity` and `sprite` are optional. A sprite names a declared texture, but the compiled plan stores only its numeric texture index.

The checked-in [`moving_sprites.json`](../tests/fixtures/scenarios/valid/moving_sprites.json) is the complete reference example. Static and texture-switching workloads are beside it.

## Built-in operations

The compiler-owned descriptor table is the only source of truth for operation metadata:

| Operation | Legal phase | Query | Reads | Writes | Complexity | Frame allocation |
|---|---|---|---|---|---|---|
| `integrate_velocity` | `fixed_update` | Transform2D, Velocity2D | Transform2D, Velocity2D | Transform2D | `linear_dense` | forbidden |
| `wrap_bounds` | `fixed_update`, `post_update` | Transform2D | Transform2D | Transform2D | `linear_dense` | forbidden |

`integrate_velocity.parameters.delta_seconds` must be positive. `wrap_bounds.parameters.bounds` contains finite `min` and `max`, with positive width and height. An optional `expected_access` object can assert `query`, `reads`, and `writes`; it is checked against the descriptor and never overrides it.

`after` contains same-phase system IDs. Unknown dependencies and cross-phase dependencies are rejected, cycles produce `IR_DEPENDENCY_CYCLE`, and same-phase overlapping writers without a transitive dependency order produce `IR_AMBIGUOUS_WRITE_ORDER`. The stable topological order uses phase order first and authoring order as its tie-break.

## Compilation and frame boundary

Compilation performs parse, schema/semantic validation, canonicalization, dependency ordering, asset resolution, cardinality estimation, and stable FNV-1a hashing. The resulting `ExecutionPlan` holds numeric operation, component, phase, system-index, and texture-index values. A separate symbol table exists only for diagnostics and inspection. Because `ExecutionPlan` is a public mutable value, the runtime validates all indices, enum domains, counts, dependency order, cardinalities, finite numeric values, and the stable hash again before reserving or mutating runtime state.

During a frame there is no JSON access, string or component lookup, parameter map, structural mutation, or per-entity callable. The runtime switches on the numeric operation once per system and enters the corresponding dense world loop. Checked double-precision intermediates reject non-finite or out-of-float-range state, and wrapping uses constant-time remainder arithmetic. Render extraction copies `Transform2D + Sprite2D` values into a pre-reserved renderer submission array; transform scale multiplies sprite size, while pivot is applied consistently by CPU culling and the vertex shader.

## Commands

```powershell
python tools/engine.py validate tests/fixtures/scenarios/valid/moving_sprites.json --preset dev --json
python tools/engine.py inspect scenario tests/fixtures/scenarios/valid/moving_sprites.json --preset dev --json
python tools/engine.py inspect plan tests/fixtures/scenarios/valid/moving_sprites.json --preset dev --json
python tools/engine.py inspect diagnostics-schema --preset dev --json
python tools/engine.py run tests/fixtures/scenarios/valid/moving_sprites.json --frames 600 --headless --preset dev --json
python tools/engine.py profile tests/fixtures/scenarios/valid/moving_sprites.json --frames 600 --preset release --json
```

Invalid fixtures under `tests/fixtures/scenarios/invalid` pin the stable codes for unknown operations, access mismatches, missing textures, dependency cycles, ambiguous writes, capacity overflow, invalid bounds, and unsupported versions.
