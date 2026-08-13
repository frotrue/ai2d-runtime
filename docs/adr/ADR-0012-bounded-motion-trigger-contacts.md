# ADR-0012: Bounded motion and trigger contacts

## Status

Accepted for v0.5.

## Context

The v0.4 runtime can reuse bounded objects, but a moving pooled projectile cannot directly observe a moving enemy without proxy objects or a transform-follow chain. The old collision contract also combines contact detection with physical response, which cannot express a non-solid zone that reports entry and exit. A general rigid-body engine would add mass, force, friction, joints, solver policy, and substantially broader nondeterminism than these games need.

Black-box input scripts can drive games, but they cannot prove exact motion, contact lifecycle, or deterministic state without bespoke test code. This weakens the AI-first goal because an independent author cannot objectively verify a declarative game using only the public tools.

## Decision

GameManifest/SceneSpec 0.5 separates each collision rule into a required `solid` or `trigger` interaction. Solid rules retain the bounded swept-AABB solver and require exactly one dynamic endpoint, one static/kinematic endpoint, and a reflect reaction. Trigger rules allow every static/kinematic/dynamic combination, perform no physical response, and emit typed `contact_begin` and `contact_end` events.

The collision core records fixed-capacity motion segments. Static objects contribute a stationary segment, authored kinematic motion contributes `previous_position -> position`, and dynamic integration contributes every reflected segment. Trigger detection performs relative swept AABB over overlapping segment time ranges. Two sorted, preallocated contact-pair arrays provide deterministic begin/end diffing and retained snapshots. Pool release/recycle/reset and other deactivation invalidate contact identity immediately.

The closed system set gains `linear_motion`, which integrates active kinematic objects with Velocity2D once per fixed tick. Compiler ownership rules prevent double integration or overlap with axis control/follower ownership.

The public `game-test-v1` format and `game verify` command compile all authored names to numeric indices before execution, evaluate bounded assertions immediately after their 0-based ticks, and compare fresh-runtime repetitions including state and contact checksums. No callback, expression language, or image-golden mechanism is introduced.

Schema 0.2 through 0.4 parsing, behavior, and hashing remain unchanged. Only 0.5 hashes include interaction, contact capacity, contact events, and linear-motion ownership.

## Alternatives

- General rigid-body physics: rejected because the required use cases need contact observation, not force-based resolution.
- Continue using collision proxies/follow chains: rejected because it obscures object identity and cannot robustly represent two independently moving endpoints.
- Per-frame overlap callbacks: rejected because callbacks violate the closed declarative runtime and do not provide bounded lifecycle semantics.
- Screenshot-only verification: rejected as the primary contract because exact state and transform assertions are cheaper, deterministic, and useful in headless CI.

## Consequences

Moving projectiles, enemies, zones, pickups, and hazards can contact one another directly with deterministic ordering and no gameplay C++. Authors must declare contact capacity, body motion, and system ownership explicitly. Solid dynamic-dynamic response remains unsupported. Trigger storage and narrowphase work are bounded by authored capacities, and all steady-state buffers are reserved at scene load.

## Evidence and revisit trigger

Pool Siege must demonstrate direct moving pooled contacts and contact-position spawning; Contact Course must demonstrate persistent begin/end without shooter semantics. Unit tests must cover all body combinations, pass-through, reflected segments, lifecycle invalidation, snapshots, capacity diagnostics, and 10,000-entity allocation-free sparse churn. Revisit only when multiple shipped games require a physical response that cannot be expressed by one-dynamic swept-AABB solids plus typed triggers.
