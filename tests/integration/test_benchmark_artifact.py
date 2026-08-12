from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path


def main() -> int:
    root = Path(sys.argv[1]).resolve()
    gpu_enabled = "--gpu-enabled" in sys.argv[2:]
    engine = root / "tools" / "engine.py"
    preset = Path.cwd().name
    with tempfile.TemporaryDirectory(prefix="ai2d-benchmark-") as temporary:
        output = Path(temporary) / "artifact.json"
        process = subprocess.run(
            [
                sys.executable,
                str(engine),
                "benchmark",
                "--suite",
                "world",
                "--counts",
                "1000",
                "--runs",
                "1",
                "--warmup",
                "1",
                "--frames",
                "3",
                "--preset",
                preset,
                "--output",
                str(output),
                "--json",
            ],
            cwd=root,
            text=True,
            capture_output=True,
            check=False,
        )
        if process.returncode != 0:
            print(process.stderr, file=sys.stderr)
            print(process.stdout, file=sys.stderr)
            return 1
        stdout_document = json.loads(process.stdout)
        file_document = json.loads(output.read_text(encoding="utf-8"))
        assert stdout_document == file_document
        assert file_document["status"] == "pass"
        fingerprint = file_document["fingerprint"]
        assert len(fingerprint["hash"]) == 64
        assert fingerprint["comparison_key"]["suite"] == "world"
        assert str(output.resolve()) in file_document["artifacts"]

        if gpu_enabled:
            scenario_output = Path(temporary) / "scenario-artifact.json"
            scenario_process = subprocess.run(
                [
                    sys.executable,
                    str(engine),
                    "benchmark",
                    "--suite",
                    "scenario",
                    "--counts",
                    "1000",
                    "--runs",
                    "1",
                    "--warmup",
                    "1",
                    "--frames",
                    "3",
                    "--preset",
                    preset,
                    "--output",
                    str(scenario_output),
                    "--json",
                ],
                cwd=root,
                text=True,
                capture_output=True,
                check=False,
            )
            if scenario_process.returncode != 0:
                print(scenario_process.stderr, file=sys.stderr)
                print(scenario_process.stdout, file=sys.stderr)
                return 1
            scenario_document = json.loads(scenario_output.read_text(encoding="utf-8"))
            assert scenario_document["status"] == "pass"
            assert len(scenario_document["metrics"]["results"]) == 5
            for result in scenario_document["metrics"]["results"]:
                memory = result["memory"]
                assert memory["frame_arena_overflows"] is None
                assert memory["frame_arena_overflows_unavailable_reason"]

        forged = Path(temporary) / "forged-empty.json"
        forged.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "command": "benchmark",
                    "status": "pass",
                    "diagnostics": [],
                    "metrics": {"suite": "all", "suites": []},
                }
            ),
            encoding="utf-8",
        )
        baseline_name = f"invalid-empty-{uuid.uuid4().hex}"
        destination = root / "artifacts" / "benchmarks" / "baselines" / f"{baseline_name}.json"
        promoted = subprocess.run(
            [
                sys.executable,
                str(engine),
                "promote-baseline",
                str(forged),
                baseline_name,
                "--json",
            ],
            cwd=root,
            text=True,
            capture_output=True,
            check=False,
        )
        try:
            promotion_document = json.loads(promoted.stdout)
            assert promoted.returncode == 2, promotion_document
            assert promotion_document["status"] == "fail"
            assert not destination.exists()
        finally:
            destination.unlink(missing_ok=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
