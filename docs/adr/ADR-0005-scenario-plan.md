# ADR-0005: ScenarioSpec and sequential ExecutionPlan boundary

- Status: accepted
- Date: 2026-08-12

## Context

AI authoring needs stable validation and inspection, while the runtime must avoid per-frame JSON and names.

## Decision

Parse a versioned JSON ScenarioSpec only at load. Canonical operation descriptors own access/query/complexity/allocation metadata. Resolve assets, systems, dependencies, phases, and parameters into numeric values and an ordered sequential plan. Keep debug names outside hot dispatch. Do not persist a binary plan or permit custom code/expressions.

## Alternatives considered

Direct gameplay C++, scripting, bytecode, code generation, and a full optimizing IR were rejected as unneeded for two operations and harmful to observability.

## Evidence and consequences

Static/moving/wrapping workloads fit deterministic spawn distributions and `integrate_velocity`/`wrap_bounds`. Expressiveness is intentionally narrow.

## Revisit trigger

A required workload needs an arbitrary escape hatch, or adding one small semantic operation duplicates metadata across layers.
