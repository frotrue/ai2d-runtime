# Contact Dodger

Contact Dodger is a source-blind, independently authored GameSpec 0.5 release-gate game.  It uses only declarative manifest/scene data and procedural project-owned PNG/font assets.

- Space fires the one-slot `skip` projectile pool.
- Tab injects a three-enemy surge into the two-slot `recycle_oldest` enemy pool.
- Arrow/A/D movement changes the projectile launch point.
- Enter or the on-screen button restarts the same scene and resets session state.
- Enemy entry into the scan gate costs health; retained separation records a meaningful gate end.
- A direct moving bullet/enemy trigger contact spawns a short-lived spark at the captured enemy contact position, then releases `collision_a` and `collision_b` to their owning pools.

Regenerate the assets with `python samples/contact_dodger/generate_assets.py`.

`tests/contact_lifecycle.json` is the primary repeatable release gate.  It checks consecutive-tick projectile motion, the exact captured spark position, pool exhaustion/release/reuse/recycling/expiration, direct trigger begins, retained trigger ends, and the win state.  `tests/loss_restart.json` separately checks loss and same-scene session restart.
