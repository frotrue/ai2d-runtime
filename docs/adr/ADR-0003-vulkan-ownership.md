# ADR-0003: raw Vulkan API with move-only ownership

- Status: accepted
- Date: 2026-08-12

## Context

The ownership spike must consider SDL surface interoperability, VMA, exception policy, compile cost, and clarity. The host initially has the loader but no SDK headers.

## Decision

Use the Vulkan C API from the pinned SDK and small backend-local move-only wrappers for VMA buffers/images and owned Vulkan objects. Link the loader normally. Isolate `VMA_IMPLEMENTATION` in one translation unit. No Vulkan-Hpp or dual implementation is retained.

## Alternatives considered

Vulkan-Hpp RAII was rejected because exception policy and mixed VMA/SDL ownership add a second lifetime model. Volk was considered for an SDK-less build but became unnecessary once a portable pinned SDK supplied headers/import library and validation tools.

## Evidence and consequences

SDL3 accepts raw `VkInstance`/`VkSurfaceKHR`, and VMA is a C API. Explicit wrappers keep destruction order visible. More boilerplate is accepted within the private backend.

## Revisit trigger

Ownership bugs persist despite validation/tests, or wrapper code becomes broader than concrete resource lifetimes.
