# ADR-0002: Vulkan baseline and abstraction level

- Status: accepted
- Date: 2026-08-12

## Context

The RTX 3060 exposes Vulkan 1.4.325, while v0.1 requires Vulkan 1.3 dynamic rendering and synchronization2. A generic graphics abstraction would exceed the workload.

## Decision

Require Vulkan 1.3, dynamic rendering, synchronization2, a graphics queue, and (for presented mode) present support. Keep one renderer-specific private Vulkan backend with two frame slots, swapchain and fixed offscreen paths. Do not create an RHI, command abstraction, render graph, or device-loss recovery machine.

## Alternatives considered

Vulkan 1.2 + extensions adds branches without host benefit; a generic RHI adds untested policy; SDL GPU would not test the requested Vulkan contracts.

## Evidence and consequences

Host capability is above the minimum. Other devices fail with `VK_DEVICE_UNSUPPORTED`. The backend is intentionally Vulkan-specific and smaller.

## Revisit trigger

A required reference device cannot support the baseline, or generic abstraction code begins to exceed renderer code.
