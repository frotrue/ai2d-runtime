from __future__ import annotations

import argparse
import json
import subprocess
import tempfile
from pathlib import Path

REQUIRED_ENVELOPE = {
    "schema_version", "command", "status", "build", "environment", "diagnostics", "metrics", "artifacts"
}


def run(binary: Path, *arguments: str) -> dict[str, object]:
    process = subprocess.run(
        [str(binary), *arguments, "--json"],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=30,
    )
    if process.returncode != 0:
        raise AssertionError(f"command failed ({process.returncode}): {process.stderr}\n{process.stdout}")
    if process.stderr:
        raise AssertionError(f"JSON command wrote stderr: {process.stderr}")
    document = json.loads(process.stdout)
    assert REQUIRED_ENVELOPE <= document.keys()
    if document.get("status") != "pass":
        raise AssertionError(document)
    return document


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("root", type=Path)
    parser.add_argument("--gpu", action="store_true")
    arguments = parser.parse_args()
    manifest = arguments.root / "samples" / "breakout" / "game.json"

    validated = run(arguments.binary, "game", "validate", str(manifest))
    assert validated["metrics"]["scene_count"] == 5  # type: ignore[index]

    inspected = run(arguments.binary, "game", "inspect", str(manifest))
    metrics = inspected["metrics"]
    assert metrics["schema_version"] == "0.2"  # type: ignore[index]
    assert metrics["transition_count"] == 11  # type: ignore[index]

    executed = run(
        arguments.binary,
        "game",
        "run",
        str(manifest),
        "--headless",
        "--frames",
        "3",
        "--no-saved-settings",
    )
    run_metrics = executed["metrics"]
    assert run_metrics["simulation_ticks"] == 3  # type: ignore[index]
    assert run_metrics["scene"] == "menu"  # type: ignore[index]

    with tempfile.TemporaryDirectory() as temporary:
        input_script = Path(temporary) / "input.json"
        input_script.write_text(
            json.dumps(
                {
                    "schema_version": "1",
                    "events": [
                        {"tick": 0, "action": "start", "kind": "tap"},
                        {"tick": 1, "action": "launch", "kind": "press"},
                        {"tick": 2, "action": "launch", "kind": "release"},
                    ],
                },
                separators=(",", ":"),
            ),
            encoding="utf-8",
        )
        scripted = run(
            arguments.binary,
            "game",
            "run",
            str(manifest),
            "--headless",
            "--frames",
            "3",
            "--input-script",
            str(input_script),
            "--no-saved-settings",
        )
        scripted_metrics = scripted["metrics"]
        assert scripted_metrics["simulation_ticks"] == 3  # type: ignore[index]
        assert Path(scripted_metrics["input_script"]) == input_script  # type: ignore[arg-type,index]
        assert scripted_metrics["scene"] == "game"  # type: ignore[index]

        interactive_rejected = subprocess.run(
            [
                str(arguments.binary),
                "game",
                "run",
                str(manifest),
                "--input-script",
                str(input_script),
                "--json",
            ],
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            timeout=5,
        )
        assert interactive_rejected.returncode == 2
        assert json.loads(interactive_rejected.stdout)["status"] == "fail"

    if arguments.gpu:
        with tempfile.TemporaryDirectory(prefix="ai2d-hidden-script-") as temporary:
            hidden_input_script = Path(temporary) / "input.json"
            hidden_input_script.write_text(
                json.dumps(
                    {
                        "schema_version": "1",
                        "events": [{"tick": 0, "action": "start", "kind": "tap"}],
                    },
                    separators=(",", ":"),
                ),
                encoding="utf-8",
            )
            hidden_scripted = run(
                arguments.binary,
                "game",
                "run",
                str(manifest),
                "--hidden",
                "--frames",
                "3",
                "--input-script",
                str(hidden_input_script),
                "--no-audio",
                "--no-saved-settings",
            )
            hidden_scripted_metrics = hidden_scripted["metrics"]
            assert hidden_scripted_metrics["simulation_ticks"] == 3  # type: ignore[index]
            assert Path(hidden_scripted_metrics["input_script"]) == hidden_input_script  # type: ignore[arg-type,index]
            assert hidden_scripted_metrics["scene"] == "game"  # type: ignore[index]

        offscreen = run(
            arguments.binary,
            "game",
            "run",
            str(manifest),
            "--offscreen",
            "--frames",
            "2",
            "--no-audio",
            "--no-saved-settings",
        )
        offscreen_metrics = offscreen["metrics"]
        assert offscreen_metrics["loaded_textures"] == 3  # type: ignore[index]
        assert offscreen_metrics["last_frame"]["sprites"] > 0  # type: ignore[index]

        rejected = subprocess.run(
            [str(arguments.binary), "game", "run", str(manifest), "--offscreen", "--json"],
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            timeout=5,
        )
        assert rejected.returncode == 2
        rejected_document = json.loads(rejected.stdout)
        assert REQUIRED_ENVELOPE <= rejected_document.keys()
        assert rejected_document["status"] == "fail"
    else:
        unavailable = subprocess.run(
            [
                str(arguments.binary),
                "game",
                "run",
                str(manifest),
                "--offscreen",
                "--frames",
                "1",
                "--no-audio",
                "--no-saved-settings",
                "--json",
            ],
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            timeout=5,
        )
        assert unavailable.returncode == 3
        assert not unavailable.stderr
        unavailable_document = json.loads(unavailable.stdout)
        assert REQUIRED_ENVELOPE <= unavailable_document.keys()
        assert unavailable_document["status"] == "unavailable"
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
