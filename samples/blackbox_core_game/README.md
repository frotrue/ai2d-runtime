# Pulse Salvage

Pulse Salvage is a one-screen, fully declarative GameManifest/SceneSpec 0.5 game. The cyan salvage probe drifts through a containment lane while a red plasma orb repeatedly crosses it and reflects from the two solid energy walls.

## Rules and controls

- Press **Space** to launch a yellow salvage packet. A packet collected by the moving probe adds one point. Reach three points to win.
- The red plasma orb removes one hull point when contact begins. Its later contact end clears the danger flag and records a successful clearance. Lose all four hull points to lose.
- Press **Down** to launch a short-lived decoy. It exists to expose the skip-pool lifetime/exhaustion mechanic during play and disappears after three fixed ticks.
- Press **Enter** on either result screen to reset the session and restart.

The game has no game-specific C++ and requires no runtime or engine modification. `generate_assets.py` creates the PNG sprite atlas and bitmap font using only Python's standard library.

## Black-box commands

```powershell
python samples/blackbox_core_game/generate_assets.py
python tools/engine.py validate samples/blackbox_core_game/game.json --preset dev --json
python tools/engine.py inspect plan samples/blackbox_core_game/game.json --preset dev --json
python tools/engine.py verify samples/blackbox_core_game/game.json --test-script samples/blackbox_core_game/tests/core-win.json --repeat 2 --preset dev --json
python tools/engine.py verify samples/blackbox_core_game/game.json --test-script samples/blackbox_core_game/tests/defeat-restart.json --repeat 2 --preset dev --json
```

The core verification also pins the first recycled effect at `[0.2, 0.027]`, proving that its transform came from the captured trigger-contact position rather than a later live transform.
