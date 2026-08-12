from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path


def main() -> int:
    root = Path(sys.argv[1]).resolve()
    suffix = ".exe" if sys.platform == "win32" else ""
    binary = Path.cwd() / "bin" / f"ai2d_cli{suffix}"
    command = [
        str(binary),
        "benchmark",
        "--suite",
        "world",
        "--counts",
        "1000",
        "--runs",
        "1",
        "--warmup",
        "5",
        "--frames",
        "10",
        "--json",
    ]
    process = subprocess.run(command, cwd=root, text=True, capture_output=True, check=False)
    if process.returncode != 0:
        print(process.stderr, file=sys.stderr)
        print(process.stdout, file=sys.stderr)
        return 1
    try:
        document = json.loads(process.stdout)
    except json.JSONDecodeError as error:
        print(f"invalid benchmark JSON: {error}", file=sys.stderr)
        return 1
    result = document["metrics"]["results"][0]
    if document["status"] != "pass" or result["entity_count"] != 1000:
        print(f"unexpected result: {document}", file=sys.stderr)
        return 1
    memory = result["memory"]
    invariant_keys = ("tracked_cpp_heap_allocations", "capacity_growth_events")
    if any(memory[key] != 0 for key in invariant_keys):
        print(f"memory invariant failed: {memory}", file=sys.stderr)
        return 1
    if memory["frame_arena_overflows"] is not None or not memory["frame_arena_overflows_unavailable_reason"]:
        print(f"frame arena availability contract failed: {memory}", file=sys.stderr)
        return 1
    for phase in ("query_transform_sprite_ms", "integrate_velocity_ms", "wrap_bounds_ms"):
        if result[phase]["sample_count"] != 10:
            print(f"missing samples for {phase}: {result[phase]}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
