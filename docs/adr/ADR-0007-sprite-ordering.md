# ADR-0007: sprite ordering and batching semantics

- Status: accepted
- Date: 2026-08-12

## Context

Strict global alpha order conflicts with texture batching, while the v0.1 workloads need predictable draw counts more than arbitrary overlap semantics.

## Decision

Render layers in ascending order. Within one layer, permit deterministic texture reordering and do not guarantee strict alpha order. Authors use separate layers for required overlap order. Batch by the single sprite pipeline, layer, and texture. Emit fragmentation diagnostics when unique batch keys exceed a documented threshold.

## Alternatives considered

Strict insertion order produces texture-switch draws; atlases and bindless descriptors are explicit anti-goals; general order groups are deferred without evidence.

## Evidence and consequences

A single-layer/single-texture workload must produce exactly one sprite batch and draw. Texture-switching workloads expose fragmentation quantitatively.

## Revisit trigger

The ordering contract repeatedly causes observable correctness failures in required scenes.
