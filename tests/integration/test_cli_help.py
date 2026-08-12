from __future__ import annotations

import subprocess
import sys
from pathlib import Path


def check(binary: Path, *arguments: str, expected: str) -> None:
    process = subprocess.run(
        [str(binary), *arguments],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=10,
    )
    if process.returncode != 0 or process.stderr or expected not in process.stdout:
        raise AssertionError(
            f"help command failed ({process.returncode}): {process.stderr!r}\n{process.stdout!r}"
        )


def main() -> int:
    binary = Path(sys.argv[1])
    check(binary, "--help", expected="usage: ai2d_cli <command>")
    check(binary, "game", "--help", expected="--input-script FILE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
