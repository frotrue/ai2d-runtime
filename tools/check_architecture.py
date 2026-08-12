"""Small repository-boundary checker; CMake target visibility remains primary."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

SCHEMA_VERSION = 1
SOURCE_SUFFIXES = {".h", ".hh", ".hpp", ".c", ".cc", ".cpp", ".cxx"}
EXCLUDED_PARTS = {"artifacts", "build", ".git", "vendor", "generated", "_deps"}

ALLOWED_PROJECT_DEPENDENCIES: dict[str, set[str]] = {
    "foundation": set(),
    "world": {"foundation"},
    "platform": {"foundation"},
    "renderer2d": {"foundation"},
    "scenario": {"foundation"},
    "runtime": {"foundation", "world", "platform", "renderer2d", "scenario"},
}

INCLUDE_PATTERN = re.compile(r"^\s*#\s*include\s*[<\"]([^>\"]+)[>\"]", re.MULTILINE)
DIRECT_ALLOCATION_PATTERN = re.compile(r"\b(?:new|delete|malloc|free)\b")


def module_for(path: Path) -> str | None:
    parts = path.as_posix().split("/")
    if len(parts) >= 3 and parts[0] == "src":
        return parts[1]
    if len(parts) >= 4 and parts[0] == "include" and parts[1] == "ai2d":
        return parts[2]
    return None


def referenced_module(include: str) -> str | None:
    parts = include.split("/")
    if len(parts) >= 2 and parts[0] == "ai2d":
        return parts[1]
    return None


def strip_comments(text: str) -> str:
    without_blocks = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//.*?$", "", without_blocks, flags=re.MULTILINE)


def violation(path: Path, rule: str, message: str, line: int | None = None) -> dict[str, object]:
    item: dict[str, object] = {
        "code": "ARCH_DEPENDENCY_VIOLATION",
        "severity": "error",
        "subsystem": "architecture",
        "message": message,
        "context": {"file": path.as_posix(), "rule": rule},
        "suggestions": ["Move the dependency behind the owning module's public value API."],
    }
    if line is not None:
        context = item["context"]
        assert isinstance(context, dict)
        context["line"] = line
    return item


def check_file(relative: Path, absolute: Path) -> list[dict[str, object]]:
    findings: list[dict[str, object]] = []
    module = module_for(relative)
    text = absolute.read_text(encoding="utf-8")
    includes = list(INCLUDE_PATTERN.finditer(text))

    for match in includes:
        include = match.group(1).replace("\\", "/")
        line = text.count("\n", 0, match.start()) + 1
        lower = include.lower()
        if ("vulkan" in lower or lower.startswith("vk_")) and not relative.as_posix().startswith(
            "src/renderer2d/vulkan/"
        ):
            findings.append(
                violation(relative, "vulkan_private", f"Vulkan include '{include}' is outside the private backend", line)
            )
        if lower.startswith("sdl3/"):
            allowed_sdl = relative.as_posix().startswith("src/platform/sdl/") or relative.as_posix() == (
                "src/renderer2d/vulkan/sdl_surface.cpp"
            )
            if not allowed_sdl:
                findings.append(
                    violation(relative, "sdl_boundary", f"SDL include '{include}' is outside platform/bridge code", line)
                )
        if lower.startswith("nlohmann/") and not relative.as_posix().startswith("src/scenario/"):
            findings.append(
                violation(relative, "json_parser_isolation", f"JSON parser include '{include}' escaped scenario", line)
            )

        dependency = referenced_module(include)
        if module in ALLOWED_PROJECT_DEPENDENCIES and dependency and dependency != module:
            if dependency not in ALLOWED_PROJECT_DEPENDENCIES[module]:
                findings.append(
                    violation(
                        relative,
                        "project_dependency_direction",
                        f"Module '{module}' may not include project module '{dependency}'",
                        line,
                    )
                )

    hot_path = relative.as_posix().startswith(("src/world/", "src/runtime/", "src/renderer2d/"))
    allowed_allocation_file = relative.as_posix() in {
        "src/renderer2d/vulkan/vma_impl.cpp",
        "src/renderer2d/vulkan/backend.cpp",
    }
    if hot_path and not allowed_allocation_file:
        stripped = strip_comments(text)
        for match in DIRECT_ALLOCATION_PATTERN.finditer(stripped):
            line = stripped.count("\n", 0, match.start()) + 1
            findings.append(
                violation(
                    relative,
                    "hot_path_direct_allocation",
                    f"Direct allocation API token '{match.group(0)}' appears in hot-path project code",
                    line,
                )
            )
    return findings


def run(root: Path) -> dict[str, object]:
    findings: list[dict[str, object]] = []
    scanned = 0
    for absolute in sorted(root.rglob("*")):
        if not absolute.is_file() or absolute.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        relative = absolute.relative_to(root)
        if any(part in EXCLUDED_PARTS or part.startswith("build-") for part in relative.parts):
            continue
        scanned += 1
        findings.extend(check_file(relative, absolute))
    return {
        "schema_version": SCHEMA_VERSION,
        "command": "check_architecture",
        "status": "pass" if not findings else "fail",
        "diagnostics": findings,
        "metrics": {"files_scanned": scanned, "violation_count": len(findings)},
        "artifacts": [],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--json", action="store_true")
    arguments = parser.parse_args()
    result = run(arguments.root.resolve())
    if arguments.json:
        print(json.dumps(result, ensure_ascii=False, separators=(",", ":"), allow_nan=False))
    elif result["status"] == "pass":
        metrics = result["metrics"]
        assert isinstance(metrics, dict)
        print(f"architecture: PASS ({metrics['files_scanned']} files)")
    else:
        metrics = result["metrics"]
        assert isinstance(metrics, dict)
        print(f"architecture: FAIL ({metrics['violation_count']} violations)", file=sys.stderr)
        diagnostics = result["diagnostics"]
        assert isinstance(diagnostics, list)
        for finding in diagnostics:
            assert isinstance(finding, dict)
            context = finding["context"]
            assert isinstance(context, dict)
            print(f"- {context['file']}: {finding['message']}", file=sys.stderr)
    return 0 if result["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
