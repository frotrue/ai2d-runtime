from __future__ import annotations

import argparse
import json
import subprocess
import tempfile
from pathlib import Path


def invoke(binary: Path, *arguments: str, expect_success: bool = True) -> dict[str, object]:
    process = subprocess.run(
        [str(binary), *arguments, "--json"],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=30,
    )
    document = json.loads(process.stdout)
    if expect_success:
        assert process.returncode == 0, (process.stderr, document)
        assert process.stderr == ""
        assert document["status"] == "pass"
    else:
        assert process.returncode != 0
        assert document["status"] == "fail"
    return document


def verify(binary: Path, manifest: Path, script: Path) -> dict[str, object]:
    document = invoke(
        binary,
        "game",
        "verify",
        str(manifest),
        "--test-script",
        str(script),
        "--repeat",
        "2",
    )
    metrics = document["metrics"]
    assert metrics["repeat"] == 2
    assert metrics["contact_state_checksum"] >= 0
    assert metrics["assertions"]
    return metrics


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("root", type=Path)
    parser.add_argument("--gpu", action="store_true")
    arguments = parser.parse_args()

    siege = arguments.root / "samples" / "pool_siege"
    siege_metrics = verify(arguments.binary, siege / "game.json", siege / "tests" / "smoke.json")
    assert siege_metrics["scene"] == "win"
    assert siege_metrics["states"]["score"] == 3
    assert siege_metrics["contact_begins"] == 3
    assert siege_metrics["linear_motion_updates"] > 400
    assert any(
        observation["tick"] == 119 and observation["actual"] >= 1
        for observation in siege_metrics["assertions"]
    )
    siege_restart = verify(
        arguments.binary,
        siege / "game.json",
        siege / "tests" / "loss-restart.json",
    )
    assert siege_restart["scene"] == "play"
    assert siege_restart["states"]["health"] == 3

    course = arguments.root / "samples" / "contact_course"
    course_metrics = verify(
        arguments.binary,
        course / "game.json",
        course / "tests" / "contact-lifecycle.json",
    )
    assert course_metrics["scene"] == "course"
    assert course_metrics["states"] == {"begins": 1, "ends": 1}
    assert course_metrics["contact_begins"] == 1
    assert course_metrics["contact_ends"] == 1
    assert course_metrics["linear_motion_updates"] == 240

    with tempfile.TemporaryDirectory(prefix="ai2d-v05-assertion-") as temporary:
        failing = Path(temporary) / "failing.json"
        source = json.loads((course / "tests" / "contact-lifecycle.json").read_text(encoding="utf-8"))
        source["assertions"][1]["expected"] = [-100, 0]
        failing.write_text(json.dumps(source, separators=(",", ":")), encoding="utf-8")
        rejected = invoke(
            arguments.binary,
            "game",
            "verify",
            str(course / "game.json"),
            "--test-script",
            str(failing),
            "--repeat",
            "2",
            expect_success=False,
        )
        assert rejected["diagnostics"][0]["code"] == "GAME_TEST_ASSERTION_FAILED"

    fixed_hashes = {
        "breakout": (9083610792174461301, 6721347758608948258),
        "snake": (17470168603721065075, 8662144749949340176),
        "grid_collector": (2351439514531659049, 8174193443164806110),
        "projectile_arena": (10559158477999424372, 2568389474762519076),
        "timed_pickups": (5690496091730965423, 1461871726252971360),
        "pool_dodger": (7397713390522847020, 13989442041582692557),
    }
    for sample, expected in fixed_hashes.items():
        inspected = invoke(
            arguments.binary,
            "game",
            "inspect",
            str(arguments.root / "samples" / sample / "game.json"),
        )["metrics"]
        assert (inspected["source_hash"], inspected["plan_hash"]) == expected

    if arguments.gpu:
        for sample in (siege, course):
            rendered = invoke(
                arguments.binary,
                "game",
                "run",
                str(sample / "game.json"),
                "--offscreen",
                "--hidden",
                "--frames",
                "2",
                "--no-audio",
                "--no-saved-settings",
            )["metrics"]
            assert rendered["last_frame"]["sprites"] > 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
