# Pool Dodger

Pool Dodger is a source-free GameSpec 0.4 sample. A two-slot hazard pool uses
`recycle_oldest`; a one-slot pickup pool uses `skip`. Ordered fixed-tick rules
force acquisition, exhaustion, expiration, collision release, and immediate
reuse. Two hazard hits end the round, and Enter, Space, or the restart control
starts a clean session.

Generate the deterministic assets:

```powershell
python samples/pool_dodger/generate_assets.py
```

Exercise the full lifecycle and restart path:

```powershell
build/dev/bin/ai2d_cli.exe game run samples/pool_dodger/game.json --headless --frames 48 --input-script samples/pool_dodger/input/pool-lifecycle-and-restart.json --no-audio --no-saved-settings --json
```
