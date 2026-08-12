# ADR-0004: fixed dense world storage

- Status: accepted
- Date: 2026-08-12

## Context

H2/H3 require explainable iteration and controllable allocation for exactly three component types.

## Decision

Use `{index,generation}` entities, a registry free list, and `DenseStorage<T>` with dense components/entities plus sparse entity-to-dense mapping. Typed two-component borrowed queries iterate the smaller store and sparse-probe the other. Query lifetime prevents all structural mutation. Reserve from ScenarioSpec capacity.

## Alternatives considered

Archetypes, runtime registration, reflection, per-entity objects, and sparse-only storage were rejected as unnecessary or contrary to dense measured loops.

## Evidence and consequences

The fixed workloads need only `Transform2D`, `Velocity2D`, and `Sprite2D`. Swap-remove means component order is not stable; deterministic behavior relies on deterministic operations, not insertion-order promises.

## Revisit trigger

Measured sparse membership dominates modest workloads or a required operation cannot use the fixed query model.
