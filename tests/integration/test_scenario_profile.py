from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> int:
    root = Path(sys.argv[1]).resolve()
    suffix = ".exe" if sys.platform == "win32" else ""
    binary = Path.cwd() / "bin" / f"ai2d_cli{suffix}"
    scenario = root / "tests" / "fixtures" / "scenarios" / "valid" / "moving_sprites.json"
    process = subprocess.run(
        [str(binary), "profile", str(scenario), "--frames", "8", "--json"],
        cwd=root,
        text=True,
        capture_output=True,
        check=False,
    )
    if process.returncode != 0:
        print(process.stderr, file=sys.stderr)
        print(process.stdout, file=sys.stderr)
        return 1
    document = json.loads(process.stdout)
    metrics = document["metrics"]
    render = metrics["render"]
    memory = metrics["memory"]
    assert document["status"] == "pass"
    assert render["extracted_sprites"] == 1000
    assert render["visible_sprites"] == 1000
    assert render["batches"] == render["sprite_draw_calls"] == render["texture_binds"] == 1
    assert memory["tracked_cpp_heap_allocations"] == 0
    assert metrics["world"]["capacity_growth_events"] == render["capacity_growth_events"] == 0
    assert render["vma_allocation_count"] >= 4
    assert render["vma_allocation_bytes"] > render["texture_bytes"]
    assert render["texture_bytes"] == 64
    assert render["instance_buffer_capacity_bytes"] == 160000
    assert memory["process_rss_bytes"] is None and memory["process_rss_unavailable_reason"]

    with tempfile.TemporaryDirectory(prefix="ai2d-profile-output-") as temporary:
        temporary_root = Path(temporary)
        raw_output = temporary_root / "frames.jsonl"
        raw_process = subprocess.run(
            [
                str(binary),
                "profile",
                str(scenario),
                "--frames",
                "2",
                "--raw-jsonl",
                str(raw_output),
                "--json",
            ],
            cwd=root,
            text=True,
            capture_output=True,
            check=False,
        )
        assert raw_process.returncode == 0, (raw_process.stdout, raw_process.stderr)
        assert len(raw_output.read_text(encoding="utf-8").splitlines()) == 2

        blocked_destination = temporary_root / "existing-destination"
        blocked_destination.mkdir()
        sentinel = blocked_destination / "sentinel.txt"
        sentinel.write_text("preserve-me", encoding="utf-8")
        failed_process = subprocess.run(
            [
                str(binary),
                "profile",
                str(scenario),
                "--frames",
                "2",
                "--raw-jsonl",
                str(blocked_destination),
                "--json",
            ],
            cwd=root,
            text=True,
            capture_output=True,
            check=False,
        )
        failed_document = json.loads(failed_process.stdout)
        assert failed_process.returncode == 2, failed_document
        assert failed_document["status"] == "fail"
        assert sentinel.read_text(encoding="utf-8") == "preserve-me"
        assert not list(temporary_root.glob("existing-destination.tmp-*"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
