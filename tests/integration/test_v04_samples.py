from __future__ import annotations

import argparse
import json
import subprocess
import tempfile
from pathlib import Path


def run(binary: Path, manifest: Path, script: Path, frames: int) -> dict[str, object]:
    process = subprocess.run(
        [
            str(binary), "game", "run", str(manifest), "--headless", "--frames", str(frames),
            "--input-script", str(script), "--no-audio", "--no-saved-settings", "--json",
        ],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=30,
    )
    if process.returncode != 0 or process.stderr:
        raise AssertionError(f"v0.4 sample run failed ({process.returncode}): {process.stderr}\n{process.stdout}")
    document = json.loads(process.stdout)
    assert document["status"] == "pass"
    return document["metrics"]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("root", type=Path)
    parser.add_argument("--gpu", action="store_true")
    arguments = parser.parse_args()

    arena_root = arguments.root / "samples" / "projectile_arena"
    arena_win = run(arguments.binary, arena_root / "game.json", arena_root / "input" / "recycle-and-score.json", 180)
    assert arena_win["scene"] == "over"
    assert arena_win["states"]["score"] == 5
    assert arena_win["pool_exhaustions"] >= 1
    assert arena_win["pool_recycled_slots"] >= 1
    assert arena_win["pool_releases"] >= 6
    assert arena_win["peak_active_pooled_entities"] == 3
    arena_repeat = run(
        arguments.binary, arena_root / "game.json",
        arena_root / "input" / "recycle-and-score.json", 180,
    )
    assert arena_repeat["plan_hash"] == arena_win["plan_hash"]
    assert arena_repeat["last_frame"]["scene_state_checksum"] == arena_win["last_frame"]["scene_state_checksum"]

    arena_restart = run(arguments.binary, arena_root / "game.json", arena_root / "input" / "recycle-and-score.json", 220)
    assert arena_restart["scene"] == "game"
    assert arena_restart["states"] == {"score": 0, "spawn_succeeded": 0, "release_succeeded": 0}

    pickup_root = arguments.root / "samples" / "timed_pickups"
    pickups = run(arguments.binary, pickup_root / "game.json", pickup_root / "input" / "skip-expire-retain.json", 180)
    assert pickups["scene"] == "game"
    assert pickups["pool_exhaustions"] >= 1
    assert pickups["pool_expirations"] >= 1
    assert pickups["pool_resets"] == 1
    assert pickups["pool_recycled_slots"] == 0
    assert pickups["peak_active_pooled_entities"] == 2
    pickups_repeat = run(
        arguments.binary, pickup_root / "game.json",
        pickup_root / "input" / "skip-expire-retain.json", 180,
    )
    assert pickups_repeat["plan_hash"] == pickups["plan_hash"]
    assert pickups_repeat["last_frame"]["scene_state_checksum"] == pickups["last_frame"]["scene_state_checksum"]

    if arguments.gpu:
        with tempfile.TemporaryDirectory(prefix="ai2d-v04-visible-pool-") as temporary:
            temporary_root = Path(temporary)
            empty_script = temporary_root / "empty.json"
            fire_script = temporary_root / "fire.json"
            empty_script.write_text('{"schema_version":"1","events":[]}', encoding="utf-8")
            fire_script.write_text(
                '{"schema_version":"1","events":[{"tick":0,"action":"fire","kind":"tap"}]}',
                encoding="utf-8",
            )

            def offscreen(script: Path) -> dict[str, object]:
                process = subprocess.run(
                    [
                        str(arguments.binary), "game", "run", str(arena_root / "game.json"),
                        "--offscreen", "--hidden", "--frames", "1", "--input-script", str(script),
                        "--no-audio", "--no-saved-settings", "--json",
                    ],
                    check=False,
                    capture_output=True,
                    text=True,
                    encoding="utf-8",
                    timeout=30,
                )
                if process.returncode != 0 or process.stderr:
                    raise AssertionError(
                        f"v0.4 offscreen visibility run failed ({process.returncode}): "
                        f"{process.stderr}\n{process.stdout}"
                    )
                return json.loads(process.stdout)["metrics"]

            inactive = offscreen(empty_script)
            active = offscreen(fire_script)
            assert active["active_pooled_entities"] == 1
            assert active["last_frame"]["sprites"] == inactive["last_frame"]["sprites"] + 1
            assert active["last_frame"]["active_colliders"] == inactive["last_frame"]["active_colliders"] + 1

    dodger_root = arguments.root / "samples" / "pool_dodger"
    dodger_script = dodger_root / "input" / "pool-lifecycle-and-restart.json"
    dodger = run(arguments.binary, dodger_root / "game.json", dodger_script, 48)
    dodger_repeat = run(arguments.binary, dodger_root / "game.json", dodger_script, 48)
    assert dodger["plan_hash"] == dodger_repeat["plan_hash"]
    assert dodger["scene"] == "play"
    assert dodger["states"]["health"] == 2
    assert dodger["states"]["score"] == 20
    assert dodger["pool_acquire_attempts"] == 16
    assert dodger["pool_acquire_successes"] == 14
    assert dodger["pool_exhaustions"] == 6
    assert dodger["pool_recycled_slots"] == 4
    assert dodger["pool_releases"] == 6
    assert dodger["pool_expirations"] == 2
    assert dodger_repeat["last_frame"]["state_checksum"] == dodger["last_frame"]["state_checksum"]
    assert (
        dodger_repeat["last_frame"]["scene_state_checksum"]
        == dodger["last_frame"]["scene_state_checksum"]
    )

    dodger_over = run(arguments.binary, dodger_root / "game.json", dodger_script, 30)
    assert dodger_over["scene"] == "game_over"
    assert dodger_over["states"]["health"] == 0
    dodger_restarted = run(arguments.binary, dodger_root / "game.json", dodger_script, 31)
    assert dodger_restarted["scene"] == "play"
    assert dodger_restarted["states"]["health"] == 2
    assert dodger_restarted["states"]["score"] == 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
