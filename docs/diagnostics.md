# Structured diagnostics

Every diagnostic contains `code`, `severity`, `subsystem`, `message`, a structured `context` object, a `suggestions` array, and `source` (an object or `null`). Frame/timestamp fields are added when relevant. Codes are stable interface values; changing their meaning requires an ADR or explicit compatibility note.

## Registered codes

| Code | Meaning |
|---|---|
| `ARCH_DEPENDENCY_VIOLATION` | Include, target, or hot-path architecture boundary was crossed. |
| `TOOL_MISSING` | Required or requested external tool/layer is not discoverable. |
| `TOOL_VERSION_UNSUPPORTED` | A tool exists but cannot satisfy the required contract. |
| `COMMAND_UNAVAILABLE` | A command implementation or its built binary is not present. |
| `INPUT_INVALID` | CLI input is malformed independently of ScenarioSpec semantics. |
| `GAME_MANIFEST_INVALID` | A GameManifest field, reference, version, or global bound is invalid. |
| `GAME_SCENE_INVALID` | A SceneSpec field, reference, ordering rule, or scene bound is invalid. |
| `GAME_POOL_INVALID` | A bounded pool declaration, target, ownership rule, or lifecycle action is invalid. |
| `GAME_COLLISION_INTERACTION_INVALID` | A v0.5 solid/trigger body, reaction, event, capacity, or motion-ownership contract is invalid. |
| `GAME_ANIMATION_INVALID` | A v0.6 animation clip, initial animation, target, action, or completion event is invalid. |
| `GAME_PREFAB_INVALID` | A v0.6 prefab file, component, override, asset, or animation reference is invalid. |
| `GAME_SAVE_INVALID` | A v0.6 save declaration, persistent-scene requirement, slot, state allowlist, or save action is invalid. |
| `GAME_TILE_FIELD_INVALID` | A v0.6 tile layer, integer field, cell source, cell bound, or aggregate capacity is invalid. |
| `GAME_INPUT_PROFILE_INVALID` | A v0.6 input profile, key/button/axis binding, deadzone, or profile action is invalid. |
| `GAME_LOCALIZATION_INVALID` | A v0.6 locale table, key set, UTF-8 value, glyph, default locale, or locale action is invalid. |
| `GAME_PRESENTATION_INVALID` | A v0.6 camera, UI anchor/stack, music action, or particle declaration is invalid. |
| `GAME_TEST_ASSERTION_FAILED` | A compiled black-box assertion did not match its observed value. |
| `GAME_TEST_NONDETERMINISTIC` | Fresh repeated verification runs produced different observations, final state, metrics, or checksums. |
| `RUNTIME_POOL_STATE_INVALID` | Active/free order, lifetime, or snapshot data for a bounded pool is corrupt. |
| `RUNTIME_CONTACT_STATE_INVALID` | A retained/runtime contact pair or lifecycle invariant is corrupt. |
| `RUNTIME_ANIMATION_STATE_INVALID` | Active animation frame, phase, direction, entity, or restored snapshot state is corrupt. |
| `RUNTIME_SAVE_STATE_INVALID` | Save I/O, header, length, checksum, plan hash, payload, or transactional restore validation failed. |
| `RUNTIME_TILE_FIELD_STATE_INVALID` | Active tile/field storage or a restored cell snapshot violates its compiled shape/range. |
| `COLLISION_CONTACT_CAPACITY_EXCEEDED` | Fixed contact, event, or motion-segment storage could not accept more data. |
| `IR_SCHEMA_INVALID` | ScenarioSpec shape/type/required field is invalid. |
| `IR_SCHEMA_VERSION_UNSUPPORTED` | `schema_version` is not supported. |
| `IR_UNKNOWN_OPERATION` | A system names an operation outside the built-in table. |
| `IR_ACCESS_MISMATCH` | An author assertion conflicts with canonical operation access. |
| `IR_DEPENDENCY_CYCLE` | System `after` edges form a cycle or illegal phase edge. |
| `IR_AMBIGUOUS_WRITE_ORDER` | Same-phase writers lack an explicit dependency order. |
| `IR_CAPACITY_EXCEEDED` | Resolved spawn cardinality exceeds declared world capacity. |
| `IR_MISSING_TEXTURE` | A sprite references an unresolved texture. |
| `IR_INVALID_BOUNDS` | Bounds are non-finite, inverted, or otherwise invalid. |
| `WORLD_STALE_ENTITY` | Entity index/generation is invalid or no longer alive. |
| `WORLD_CAPACITY_EXCEEDED` | Entity/component reserve contract cannot accept a create. |
| `WORLD_STRUCTURAL_MUTATION_DURING_QUERY` | Create/destroy/add/remove was requested during a borrowed query. |
| `MEM_FRAME_ARENA_OVERFLOW` | Fixed frame arena could not satisfy an allocation. |
| `MEM_FRAME_HEAP_ALLOCATION` | Tracked C++ heap allocation occurred in a measured frame. |
| `MEM_CAPACITY_GROWTH` | A reserved hot-path container changed capacity in measurement. |
| `RENDER_INVALID_TEXTURE_HANDLE` | Texture handle index/generation is stale or invalid. |
| `RENDER_BATCH_FRAGMENTATION` | Layer/texture keys exceeded the documented fragmentation threshold. |
| `RENDER_UPLOAD_CAPACITY_EXCEEDED` | Instance upload would exceed preallocated frame-slot capacity. |
| `VK_DEVICE_UNSUPPORTED` | Vulkan version/features/queues required by v0.1 are unavailable. |
| `VK_VALIDATION` | Structured standard/synchronization validation callback message. |
| `VK_SWAPCHAIN_ERROR` | Acquire/present/recreation failed outside normal out-of-date handling. |
| `PERF_BASELINE_INCOMPARABLE` | Benchmark fingerprints are incompatible. |
| `PERF_REGRESSION` | Comparable result crossed a robust regression threshold. |
| `PERF_NOISE_TOO_HIGH` | Dispersion prevents a reliable performance conclusion. |
| `INTERNAL_ERROR` | An invariant or unexpected implementation failure occurred. |

## Command envelope and exits

Machine-readable commands emit:

```json
{
  "schema_version": 1,
  "command": "doctor",
  "status": "pass",
  "build": {},
  "environment": {},
  "diagnostics": [],
  "metrics": {},
  "artifacts": []
}
```

Unavailable numeric metrics are represented as `null` together with a reason field; NaN and Infinity are never emitted. Exit codes are 0 normal/PASS, 1 validation/test/benchmark failure, 2 invalid command/input, 3 unavailable required capability/tool, and 4 internal error.
