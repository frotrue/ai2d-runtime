from __future__ import annotations

import argparse
import json
import subprocess
import tempfile
from pathlib import Path


def run(binary: Path, manifest: Path, frames: int, input_script: Path | None = None) -> dict[str, object]:
    command = [
        str(binary), "game", "run", str(manifest), "--headless", "--frames", str(frames),
        "--no-audio", "--no-saved-settings", "--json",
    ]
    if input_script is not None:
        command[5:5] = ["--input-script", str(input_script)]
    process = subprocess.run(command, check=False, capture_output=True, text=True, encoding="utf-8", timeout=30)
    if process.returncode != 0 or process.stderr:
        raise AssertionError(f"sample run failed ({process.returncode}): {process.stderr}\n{process.stdout}")
    result = json.loads(process.stdout)
    assert result["status"] == "pass"
    return result["metrics"]


def write_full_board_script(path: Path) -> None:
    pattern = (
        (18, "up"), (24, "left"), (60, "up"), (66, "right"),
        (102, "up"), (108, "left"), (150, "down"), (192, "right"),
        (234, "up"), (240, "left"), (276, "up"), (282, "right"),
        (318, "up"), (324, "left"), (360, "up"), (366, "right"),
    )
    events = [
        {"tick": cycle * 384 + tick, "action": action, "kind": "tap"}
        for cycle in range(32)
        for tick, action in pattern
    ]
    path.write_text(json.dumps({"schema_version": "1", "events": events}, separators=(",", ":")), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("root", type=Path)
    arguments = parser.parse_args()

    snake = arguments.root / "samples" / "snake" / "game.json"
    snake_script = arguments.root / "samples" / "snake" / "input" / "grow-and-turn.json"
    reverse_script = arguments.root / "samples" / "snake" / "input" / "reverse-rejected.json"
    collector = arguments.root / "samples" / "grid_collector" / "game.json"
    collector_script = arguments.root / "samples" / "grid_collector" / "input" / "smoke.json"

    reverse = run(arguments.binary, snake, 6, reverse_script)
    assert reverse["scene"] == "game"
    assert reverse["rejected_direction_changes"] == 1
    assert reverse["grid_steps"] == 1
    assert reverse["follower_updates"] == 63

    self_collision = run(arguments.binary, snake, 30, snake_script)
    assert self_collision["scene"] == "lose"
    assert self_collision["states"] == {"score": 10, "length": 0, "food_available": 1}
    restarted_snake = run(arguments.binary, snake, 35, snake_script)
    assert restarted_snake["scene"] == "game"
    assert restarted_snake["states"] == {"score": 0, "length": 2, "food_available": 1}

    collector_win = run(arguments.binary, collector, 150, collector_script)
    assert collector_win["scene"] == "win"
    assert collector_win["states"] == {"score": 5, "hazard_count": 5, "alive": 1, "gem_available": 1}
    restarted_collector = run(arguments.binary, collector, 160, collector_script)
    assert restarted_collector["scene"] == "game"
    assert restarted_collector["states"] == {"score": 1, "hazard_count": 1, "alive": 1, "gem_available": 1}
    assert restarted_collector["follower_updates"] == 0

    with tempfile.TemporaryDirectory() as temporary:
        full_board_script = Path(temporary) / "full-board.json"
        write_full_board_script(full_board_script)
        full_board = run(arguments.binary, snake, 12_000, full_board_script)
    assert full_board["scene"] == "win"
    assert full_board["states"] == {"score": 610, "length": 63, "food_available": 0}
    assert full_board["relocations"] == 61
    assert full_board["active_state_changes"] == 62
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
