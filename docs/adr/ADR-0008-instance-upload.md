# ADR-0008: persistently mapped instance upload

- Status: accepted
- Date: 2026-08-12

## Context

The renderer needs a simple allocation-free per-frame instance path before optimization evidence exists.

## Decision

Allocate one VMA-backed, host-visible, persistently mapped instance region per frame slot at load/warm-up capacity. Copy contiguous instances, flush through VMA as required, and fail with `RENDER_UPLOAD_CAPACITY_EXCEEDED` instead of growing during measurement. Descriptors are prepared at texture load, not per frame.

## Alternatives considered

Per-frame staging allocations are invalid; a device-local staging pipeline adds synchronization/ownership complexity; dynamic buffer growth violates H3.

## Evidence and consequences

The initial path is small and measurable. It may be slower than device-local memory on some devices, but no optimization claim is made before benchmark data.

## Revisit trigger

Upload/submission timing is a clear bottleneck after variance-controlled measurement.
