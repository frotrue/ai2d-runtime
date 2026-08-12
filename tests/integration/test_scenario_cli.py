from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path


def invoke(binary: Path, root: Path, *arguments: str) -> tuple[int, dict[str, object]]:
    process = subprocess.run(
        [str(binary), *arguments, "--json"], cwd=root, text=True, capture_output=True, check=False
    )
    try:
        document = json.loads(process.stdout)
    except json.JSONDecodeError as error:
        raise AssertionError(f"invalid JSON for {arguments}: {error}; stderr={process.stderr}") from error
    return process.returncode, document


def main() -> int:
    root = Path(sys.argv[1]).resolve()
    suffix = ".exe" if sys.platform == "win32" else ""
    binary = Path.cwd() / "bin" / f"ai2d_cli{suffix}"
    fixture_root = root / "tests" / "fixtures" / "scenarios"
    schema = json.loads((root / "schemas" / "scenario-v0.1.schema.json").read_text(encoding="utf-8"))
    assert schema["properties"]["schema_version"]["const"] == "0.1"
    definitions = schema["$defs"]

    def check_refs(value: object) -> None:
        if isinstance(value, dict):
            reference = value.get("$ref")
            if isinstance(reference, str) and reference.startswith("#/$defs/"):
                assert reference.removeprefix("#/$defs/") in definitions
            for child in value.values():
                check_refs(child)
        elif isinstance(value, list):
            for child in value:
                check_refs(child)

    check_refs(schema)

    for name in ("static_sprites.json", "moving_sprites.json", "texture_switching.json"):
        code, document = invoke(binary, root, "validate", str(fixture_root / "valid" / name))
        assert code == 0 and document["status"] == "pass", document

    invalid = {
        "unknown_operation.json": "IR_UNKNOWN_OPERATION",
        "invalid_access.json": "IR_ACCESS_MISMATCH",
        "missing_texture.json": "IR_MISSING_TEXTURE",
        "dependency_cycle.json": "IR_DEPENDENCY_CYCLE",
        "ambiguous_write_order.json": "IR_AMBIGUOUS_WRITE_ORDER",
        "capacity_exceeded.json": "IR_CAPACITY_EXCEEDED",
        "invalid_bounds.json": "IR_INVALID_BOUNDS",
        "unsupported_version.json": "IR_SCHEMA_VERSION_UNSUPPORTED",
    }
    for name, expected in invalid.items():
        code, document = invoke(binary, root, "validate", str(fixture_root / "invalid" / name))
        diagnostics = document["diagnostics"]
        assert code == 1 and document["status"] == "fail" and diagnostics[0]["code"] == expected, document

    moving = fixture_root / "valid" / "moving_sprites.json"
    code, document = invoke(binary, root, "inspect", "plan", str(moving))
    systems = document["metrics"]["ordered_systems"]
    assert code == 0 and [item["operation"] for item in systems] == ["integrate_velocity", "wrap_bounds"]
    assert systems[1]["after_indices"] == [0]

    code, document = invoke(binary, root, "run", str(moving), "--frames", "5", "--headless")
    metrics = document["metrics"]
    assert code == 0 and document["status"] == "pass", document
    assert metrics["world"]["alive_entities"] == 1000
    assert metrics["render"]["extracted_sprites"] == 1000
    assert metrics["memory"]["tracked_cpp_heap_allocations"] == 0

    code, document = invoke(binary, root, "inspect", "diagnostics-schema")
    assert code == 0 and "IR_DEPENDENCY_CYCLE" in document["metrics"]["scenario_codes"]

    invalid_commands = (
        ("benchmark", "--runs", "not-a-number"),
        ("run", str(moving), "--frames", "not-a-number"),
        ("profile",),
        ("inspect", "unknown-kind"),
        ("validate", str(moving), "--unknown"),
        ("unknown-command",),
    )
    for arguments in invalid_commands:
        code, document = invoke(binary, root, *arguments)
        assert code == 2, (arguments, document)
        assert document["status"] == "fail", (arguments, document)
        assert document["diagnostics"][0]["code"] == "INPUT_INVALID", (arguments, document)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
