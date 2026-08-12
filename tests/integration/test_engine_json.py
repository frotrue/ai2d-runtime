from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path


def main() -> int:
    root = Path(sys.argv[1]).resolve()
    gpu_disabled = len(sys.argv) > 2 and sys.argv[2] == "--gpu-disabled"
    build_dir = Path.cwd()
    suffix = ".exe" if sys.platform == "win32" else ""
    sandbox = build_dir / "bin" / f"ai2d_sandbox{suffix}"
    if not sandbox.is_file():
        print(f"sandbox binary missing: {sandbox}", file=sys.stderr)
        return 1

    process = subprocess.run(
        [str(sandbox), "--offscreen", "--hidden", "--frames", "4", "--json"],
        cwd=root,
        text=True,
        capture_output=True,
        check=False,
    )
    expected_returncode = 3 if gpu_disabled else 0
    if process.returncode != expected_returncode:
        print(process.stderr, file=sys.stderr)
        return 1
    try:
        document = json.loads(process.stdout)
    except json.JSONDecodeError as error:
        print(f"invalid sandbox JSON: {error}", file=sys.stderr)
        return 1
    required = {"schema_version", "command", "status", "build", "diagnostics", "metrics", "artifacts"}
    if not required.issubset(document):
        print(f"missing fields: {sorted(required - set(document))}", file=sys.stderr)
        return 1
    expected_status = "unavailable" if gpu_disabled else "pass"
    if document["status"] != expected_status or document["command"] != "sandbox":
        print(f"unexpected envelope: {document}", file=sys.stderr)
        return 1
    if gpu_disabled:
        diagnostics = document.get("diagnostics", [])
        if not diagnostics or diagnostics[0].get("code") != "COMMAND_UNAVAILABLE":
            print(f"missing GPU-disabled diagnostic: {document}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
