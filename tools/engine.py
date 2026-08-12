"""Dependency-free command spine for build, validation, execution, and evidence."""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import platform
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path
from typing import Any, Iterable, Sequence

from benchmark_compare import (
    attach_comparison,
    baseline_eligible,
    strict_json_loads,
    validate_benchmark_document,
)

ROOT = Path(__file__).resolve().parents[1]
ARTIFACTS = ROOT / "artifacts"
TOOLCHAIN = ARTIFACTS / "toolchain"
DOWNLOADS = TOOLCHAIN / "downloads"
SCHEMA_VERSION = 1

MAXIMUM_ASSET_SOURCE_BYTES = 64 * 1024 * 1024
MAXIMUM_ASSET_DECODED_BYTES = 256 * 1024 * 1024
MAXIMUM_PACKAGE_SOURCE_BYTES = 512 * 1024 * 1024
MAXIMUM_PACKAGE_DECODED_ASSET_BYTES = 512 * 1024 * 1024

VULKAN_SDK_VERSION = "1.4.350.0"
VULKAN_SDK_URL = (
    f"https://sdk.lunarg.com/sdk/download/{VULKAN_SDK_VERSION}/windows/vulkan_sdk.exe?Human=true"
)
VULKAN_SDK_SHA256 = "855b27ba05d2d8119c5114c5d4ff870ca38f2c632b11e1bb9923b9b7e6ecfe7b"
SLANG_VERSION = "2026.14.1"
SLANG_URL = (
    "https://github.com/shader-slang/slang/releases/download/"
    f"v{SLANG_VERSION}/slang-{SLANG_VERSION}-windows-x86_64.zip"
)
SLANG_SHA256 = "5ed0a59d650a0af0aca45d5db4e083b3d8fb5cea05748747dd95dfbe9c580658"

EXIT_PASS = 0
EXIT_FAIL = 1
EXIT_INVALID = 2
EXIT_UNAVAILABLE = 3
EXIT_INTERNAL = 4


def diagnostic(
    code: str,
    severity: str,
    subsystem: str,
    message: str,
    *,
    context: dict[str, Any] | None = None,
    suggestions: Iterable[str] = (),
) -> dict[str, Any]:
    return {
        "code": code,
        "severity": severity,
        "subsystem": subsystem,
        "message": message,
        "context": context or {},
        "suggestions": list(suggestions),
        "source": None,
    }


def envelope(
    command: str,
    status: str,
    *,
    diagnostics: Iterable[dict[str, Any]] = (),
    metrics: dict[str, Any] | None = None,
    artifacts: Iterable[str] = (),
    build: dict[str, Any] | None = None,
    environment: dict[str, Any] | None = None,
) -> dict[str, Any]:
    return {
        "schema_version": SCHEMA_VERSION,
        "command": command,
        "status": status,
        "build": build or {},
        "environment": environment or {},
        "diagnostics": list(diagnostics),
        "metrics": metrics or {},
        "artifacts": list(artifacts),
    }


def emit(document: dict[str, Any], json_mode: bool) -> None:
    if json_mode:
        print(json.dumps(document, ensure_ascii=False, separators=(",", ":"), allow_nan=False))
        return
    status = str(document.get("status", "internal_error")).upper()
    print(f"{document.get('command', 'engine')}: {status}")
    for item in document.get("diagnostics", []):
        print(f"[{item['severity']}] {item['code']}: {item['message']}", file=sys.stderr)
    metrics = document.get("metrics", {})
    if metrics:
        print(json.dumps(metrics, ensure_ascii=False, indent=2, allow_nan=False))


def command_text(command: Sequence[str]) -> str:
    return subprocess.list2cmdline([str(part) for part in command])


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def find_vsdevcmd() -> Path | None:
    configured = os.environ.get("VSDEVCMD")
    candidates: list[Path] = []
    if configured:
        candidates.append(Path(configured))
    program_files_x86 = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"))
    visual_studio = program_files_x86 / "Microsoft Visual Studio" / "2022"
    for edition in ("BuildTools", "Community", "Professional", "Enterprise"):
        candidates.append(visual_studio / edition / "Common7" / "Tools" / "VsDevCmd.bat")
    return next((path for path in candidates if path.is_file()), None)


def windows_short_path(path: Path) -> str:
    if os.name != "nt":
        return str(path)
    buffer = ctypes.create_unicode_buffer(32768)
    length = ctypes.windll.kernel32.GetShortPathNameW(str(path), buffer, len(buffer))
    if length == 0 or length >= len(buffer):
        return str(path)
    return buffer.value


def capture_vs_environment() -> tuple[dict[str, str], str | None]:
    vsdevcmd = find_vsdevcmd()
    if vsdevcmd is None:
        return {}, None
    comspec = os.environ.get("COMSPEC", r"C:\Windows\System32\cmd.exe")
    batch_command = f"call {windows_short_path(vsdevcmd)} -arch=x64 -host_arch=x64 -no_logo >nul && set"
    process = subprocess.run(
        [comspec, "/d", "/c", batch_command],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
        encoding="mbcs" if os.name == "nt" else "utf-8",
        errors="replace",
    )
    if process.returncode != 0:
        return {}, str(vsdevcmd)
    environment: dict[str, str] = {}
    for line in process.stdout.splitlines():
        if "=" not in line:
            continue
        name, value = line.split("=", 1)
        if name:
            environment["PATH" if name.casefold() == "path" else name] = value
    return environment, str(vsdevcmd)


def find_cmake() -> Path | None:
    path = shutil.which("cmake")
    if path:
        return Path(path)
    vsdev = find_vsdevcmd()
    if vsdev:
        candidate = (
            vsdev.parents[2]
            / "Common7"
            / "IDE"
            / "CommonExtensions"
            / "Microsoft"
            / "CMake"
            / "CMake"
            / "bin"
            / "cmake.exe"
        )
        if candidate.is_file():
            return candidate
    fallback = Path(
        r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
        r"\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    )
    return fallback if fallback.is_file() else None


def find_ninja() -> Path | None:
    path = shutil.which("ninja")
    if path:
        return Path(path)
    vsdev = find_vsdevcmd()
    if vsdev:
        candidate = (
            vsdev.parents[2]
            / "Common7"
            / "IDE"
            / "CommonExtensions"
            / "Microsoft"
            / "CMake"
            / "Ninja"
            / "ninja.exe"
        )
        if candidate.is_file():
            return candidate
    fallback = Path(
        r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
        r"\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
    )
    return fallback if fallback.is_file() else None


def portable_sdk_root() -> Path:
    return TOOLCHAIN / "vulkan-sdk" / VULKAN_SDK_VERSION


def find_vulkan_sdk() -> Path | None:
    candidates = [
        Path(os.environ["VULKAN_SDK"]) if os.environ.get("VULKAN_SDK") else None,
        portable_sdk_root(),
        Path("C:/VulkanSDK") / VULKAN_SDK_VERSION,
    ]
    for candidate in candidates:
        if candidate and (candidate / "Include" / "vulkan" / "vulkan.h").is_file():
            return candidate
    return None


def find_slangc() -> Path | None:
    path = shutil.which("slangc")
    if path:
        return Path(path)
    slang_root = TOOLCHAIN / "slang" / SLANG_VERSION
    if slang_root.is_dir():
        matches = sorted(slang_root.rglob("slangc.exe" if os.name == "nt" else "slangc"))
        if matches:
            return matches[0]
    sdk = find_vulkan_sdk()
    if sdk:
        candidate = sdk / "Bin" / ("slangc.exe" if os.name == "nt" else "slangc")
        if candidate.is_file():
            return candidate
    return None


def find_spirv_val() -> Path | None:
    path = shutil.which("spirv-val")
    if path:
        return Path(path)
    sdk = find_vulkan_sdk()
    if sdk:
        candidate = sdk / "Bin" / ("spirv-val.exe" if os.name == "nt" else "spirv-val")
        if candidate.is_file():
            return candidate
    return None


def build_environment() -> tuple[dict[str, str], dict[str, Any]]:
    environment = dict(os.environ)
    vs_environment, vsdevcmd = capture_vs_environment()
    environment.update(vs_environment)
    path_entries: list[str] = []
    cmake = find_cmake()
    ninja = find_ninja()
    if cmake:
        path_entries.append(str(cmake.parent))
    if ninja:
        path_entries.append(str(ninja.parent))
    sdk = find_vulkan_sdk()
    if sdk:
        environment["VULKAN_SDK"] = str(sdk)
        sdk_bin = sdk / "Bin"
        path_entries.append(str(sdk_bin))
        # This is a copy-only SDK install with no registry integration. Using
        # VK_LAYER_PATH makes the loader use the pinned manifests directly and
        # avoids a misleading registry-lookup warning in validation output.
        environment["VK_LAYER_PATH"] = str(sdk_bin)
    slangc = find_slangc()
    if slangc:
        path_entries.insert(0, str(slangc.parent))
        environment["AI2D_SLANGC"] = str(slangc)
    spirv_val = find_spirv_val()
    if spirv_val:
        environment["AI2D_SPIRV_VAL"] = str(spirv_val)
    environment["PATH"] = os.pathsep.join(path_entries + [environment.get("PATH", "")])
    metadata = {
        "vsdevcmd": vsdevcmd,
        "vulkan_sdk": str(sdk) if sdk else None,
        "slangc": str(slangc) if slangc else None,
        "spirv_val": str(spirv_val) if spirv_val else None,
    }
    return environment, metadata


def run_capture(
    command: Sequence[str], environment: dict[str, str], timeout: float = 30.0
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(part) for part in command],
        cwd=ROOT,
        env=environment,
        text=True,
        capture_output=True,
        check=False,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
    )


def version_line(tool: Path | None, arguments: Sequence[str], environment: dict[str, str]) -> str | None:
    if tool is None:
        return None
    try:
        process = run_capture([str(tool), *arguments], environment)
    except (OSError, subprocess.TimeoutExpired):
        return None
    output = (process.stdout + "\n" + process.stderr).strip().splitlines()
    return output[0].strip() if output else None


def git_metadata(environment: dict[str, str]) -> dict[str, Any]:
    commit = run_capture(["git", "rev-parse", "HEAD"], environment)
    status = run_capture(["git", "status", "--porcelain"], environment)
    return {
        "commit": commit.stdout.strip() if commit.returncode == 0 else None,
        "working_tree": "dirty" if status.stdout.strip() else "clean",
    }


def command_doctor(_: argparse.Namespace) -> tuple[dict[str, Any], int]:
    environment, discovery = build_environment()
    cmake = find_cmake()
    ninja = find_ninja()
    compiler = shutil.which("cl", path=environment.get("PATH"))
    vulkaninfo_path = shutil.which("vulkaninfo", path=environment.get("PATH"))
    vulkaninfo = Path(vulkaninfo_path) if vulkaninfo_path else None
    slangc = find_slangc()
    spirv_val = find_spirv_val()
    sdk = find_vulkan_sdk()
    diagnostics: list[dict[str, Any]] = []

    required = {
        "cmake": str(cmake) if cmake else None,
        "ninja": str(ninja) if ninja else None,
        "compiler": compiler,
        "python": sys.executable,
    }
    for name, path in required.items():
        if not path:
            diagnostics.append(
                diagnostic(
                    "TOOL_MISSING",
                    "error",
                    "tooling",
                    f"Required build tool '{name}' was not found",
                    context={"tool": name},
                    suggestions=["Install Visual Studio 2022 Build Tools or add the tool to PATH."],
                )
            )

    optional_tools = {
        "vulkan_sdk": str(sdk) if sdk else None,
        "vulkaninfo": str(vulkaninfo) if vulkaninfo else None,
        "slangc": str(slangc) if slangc else None,
        "spirv-val": str(spirv_val) if spirv_val else None,
    }
    for name, path in optional_tools.items():
        if not path:
            diagnostics.append(
                diagnostic(
                    "TOOL_MISSING",
                    "warning",
                    "tooling",
                    f"GPU tool '{name}' is unavailable",
                    context={"tool": name},
                    suggestions=["Run 'python tools/engine.py bootstrap --json' for the pinned portable toolchain."],
                )
            )

    validation_layer = False
    device: dict[str, Any] = {}
    if vulkaninfo:
        try:
            process = run_capture([str(vulkaninfo), "--summary"], environment, timeout=60.0)
            vulkan_summary = process.stdout + process.stderr
            validation_layer = "VK_LAYER_KHRONOS_validation" in vulkan_summary
            instance_match = re.search(r"Vulkan Instance Version:\s*([0-9.]+)", vulkan_summary)
            device_name = re.search(r"deviceName\s*=\s*(.+)", vulkan_summary)
            api_version = re.search(r"apiVersion\s*=\s*([0-9.]+)", vulkan_summary)
            driver = re.search(r"driverInfo\s*=\s*(.+)", vulkan_summary)
            device = {
                "instance_version": instance_match.group(1).strip() if instance_match else None,
                "name": device_name.group(1).strip() if device_name else None,
                "api_version": api_version.group(1).strip() if api_version else None,
                "driver": driver.group(1).strip() if driver else None,
            }
        except (OSError, subprocess.TimeoutExpired) as error:
            diagnostics.append(
                diagnostic(
                    "TOOL_VERSION_UNSUPPORTED",
                    "warning",
                    "vulkan",
                    "vulkaninfo did not complete",
                    context={"error": str(error)},
                )
            )
    if not validation_layer:
        diagnostics.append(
            diagnostic(
                "TOOL_MISSING",
                "warning",
                "vulkan",
                "VK_LAYER_KHRONOS_validation is unavailable",
                context={"layer": "VK_LAYER_KHRONOS_validation"},
                suggestions=["Use the pinned portable Vulkan SDK and VK_ADD_LAYER_PATH emitted by this wrapper."],
            )
        )

    core_missing = any(item["severity"] == "error" for item in diagnostics)
    gpu_missing = any(item["severity"] == "warning" for item in diagnostics)
    status = "fail" if core_missing else ("unavailable" if gpu_missing else "pass")
    exit_code = EXIT_FAIL if core_missing else (EXIT_UNAVAILABLE if gpu_missing else EXIT_PASS)
    metrics = {
        "tools": {
            **required,
            **optional_tools,
            "cmake_version": version_line(cmake, ["--version"], environment),
            "ninja_version": version_line(ninja, ["--version"], environment),
            "compiler_version": version_line(Path(compiler) if compiler else None, [], environment),
            "python_version": platform.python_version(),
            "slang_version": version_line(slangc, ["-version"], environment),
            "spirv_tools_version": version_line(spirv_val, ["--version"], environment),
        },
        "validation_layer_available": validation_layer,
        "vulkan_device": device,
        "discovery": discovery,
    }
    return (
        envelope(
            "doctor",
            status,
            diagnostics=diagnostics,
            metrics=metrics,
            build=git_metadata(environment),
            environment={
                "os": platform.platform(),
                "architecture": platform.machine(),
                "cpu": platform.processor(),
            },
        ),
        exit_code,
    )


def download_verified(url: str, expected_sha: str, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.is_file():
        actual = sha256(destination)
        if actual == expected_sha:
            print(f"using verified download {destination}", file=sys.stderr)
            return
        raise RuntimeError(
            f"Existing download hash mismatch for {destination}: expected {expected_sha}, got {actual}; "
            "remove or quarantine the file manually before retrying."
        )
    partial = destination.with_suffix(destination.suffix + ".part")
    print(f"downloading {url}", file=sys.stderr)
    request = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 game_runtime_dev-bootstrap/0.1"})
    with urllib.request.urlopen(request, timeout=60) as response, partial.open("wb") as stream:
        total_size = int(response.headers.get("Content-Length", "0"))
        downloaded = 0
        last_report = -1
        while chunk := response.read(1024 * 1024):
            stream.write(chunk)
            downloaded += len(chunk)
            if total_size > 0:
                percent = min(100, int((downloaded * 100) / total_size))
                bucket = percent // 10
                if bucket != last_report:
                    last_report = bucket
                    print(f"download progress: {percent}%", file=sys.stderr)
    actual = sha256(partial)
    if actual != expected_sha:
        raise RuntimeError(f"Downloaded hash mismatch: expected {expected_sha}, got {actual} ({partial})")
    partial.replace(destination)


def extract_zip_verified(archive: Path, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    root = destination.resolve()
    with zipfile.ZipFile(archive) as package:
        for member in package.infolist():
            target = (destination / member.filename).resolve()
            try:
                target.relative_to(root)
            except ValueError as error:
                raise RuntimeError(f"Unsafe zip member: {member.filename}") from error
        package.extractall(destination)


def command_bootstrap(arguments: argparse.Namespace) -> tuple[dict[str, Any], int]:
    if os.name != "nt":
        return (
            envelope(
                "bootstrap",
                "unavailable",
                diagnostics=[
                    diagnostic(
                        "TOOL_VERSION_UNSUPPORTED",
                        "error",
                        "tooling",
                        "The current bootstrap recipe is pinned for Windows x64",
                        context={"os": os.name},
                    )
                ],
            ),
            EXIT_UNAVAILABLE,
        )
    start = time.perf_counter()
    installed: list[str] = []
    diagnostics: list[dict[str, Any]] = []
    try:
        if not arguments.skip_slang:
            slang_archive = DOWNLOADS / f"slang-{SLANG_VERSION}-windows-x86_64.zip"
            slang_root = TOOLCHAIN / "slang" / SLANG_VERSION
            download_verified(SLANG_URL, SLANG_SHA256, slang_archive)
            if not any(slang_root.rglob("slangc.exe")):
                extract_zip_verified(slang_archive, slang_root)
            slangc = find_slangc()
            if slangc is None:
                raise RuntimeError(f"Slang archive did not provide slangc under {slang_root}")
            installed.append(str(slangc))

        if not arguments.skip_sdk:
            sdk_root = portable_sdk_root()
            if not (sdk_root / "Include" / "vulkan" / "vulkan.h").is_file():
                installer = DOWNLOADS / f"vulkan-sdk-{VULKAN_SDK_VERSION}.exe"
                download_verified(VULKAN_SDK_URL, VULKAN_SDK_SHA256, installer)
                sdk_root.parent.mkdir(parents=True, exist_ok=True)
                install_command = [
                    str(installer),
                    "--root",
                    str(sdk_root),
                    "--accept-licenses",
                    "--default-answer",
                    "--confirm-command",
                    "install",
                    "copy_only=1",
                ]
                print(command_text(install_command), file=sys.stderr)
                creation_flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
                process = subprocess.run(
                    install_command,
                    cwd=ROOT,
                    check=False,
                    creationflags=creation_flags,
                    timeout=1200,
                )
                if process.returncode != 0:
                    raise RuntimeError(f"Vulkan SDK installer failed with exit code {process.returncode}")
            sdk = find_vulkan_sdk()
            if sdk is None:
                raise RuntimeError(f"Vulkan SDK headers were not found under {sdk_root}")
            installed.append(str(sdk))
    except (OSError, RuntimeError, subprocess.TimeoutExpired, urllib.error.URLError, zipfile.BadZipFile) as error:
        diagnostics.append(
            diagnostic(
                "TOOL_MISSING",
                "error",
                "tooling",
                "Portable toolchain bootstrap failed",
                context={"error": str(error)},
                suggestions=["Inspect the verified downloads under artifacts/toolchain/downloads and retry."],
            )
        )
        return (
            envelope(
                "bootstrap",
                "fail",
                diagnostics=diagnostics,
                metrics={"duration_seconds": time.perf_counter() - start},
                artifacts=installed,
            ),
            EXIT_FAIL,
        )
    return (
        envelope(
            "bootstrap",
            "pass",
            metrics={
                "duration_seconds": time.perf_counter() - start,
                "vulkan_sdk_version": VULKAN_SDK_VERSION,
                "slang_version": SLANG_VERSION,
            },
            artifacts=installed,
        ),
        EXIT_PASS,
    )


def configure_and_build(
    preset: str, target: str | None = None, *, fresh: bool = False
) -> tuple[dict[str, Any], int]:
    environment, discovery = build_environment()
    cmake = find_cmake()
    if cmake is None:
        document = envelope(
            "build",
            "unavailable",
            diagnostics=[
                diagnostic(
                    "TOOL_MISSING",
                    "error",
                    "build",
                    "CMake is unavailable",
                    suggestions=["Install Visual Studio 2022 Build Tools with CMake support."],
                )
            ],
        )
        return document, EXIT_UNAVAILABLE

    configure_command = [str(cmake)]
    if fresh:
        configure_command.append("--fresh")
    configure_command.extend(["--preset", preset])
    commands = [configure_command]
    build_command = [str(cmake), "--build", "--preset", preset, "--parallel"]
    if target:
        build_command.extend(["--target", target])
    commands.append(build_command)
    start = time.perf_counter()
    for command in commands:
        print(command_text(command), file=sys.stderr)
        process = subprocess.run(command, cwd=ROOT, env=environment, stdout=sys.stderr, stderr=sys.stderr, check=False)
        if process.returncode != 0:
            return (
                envelope(
                    "build",
                    "fail",
                    diagnostics=[
                        diagnostic(
                            "INTERNAL_ERROR",
                            "error",
                            "build",
                            "Configure or build command failed",
                            context={"command": command_text(command), "exit_code": process.returncode},
                        )
                    ],
                    metrics={
                        "preset": preset,
                        "duration_seconds": time.perf_counter() - start,
                        "commands": [command_text(item) for item in commands],
                        "discovery": discovery,
                    },
                ),
                EXIT_FAIL,
            )
    build_dir = ROOT / "build" / preset
    return (
        envelope(
            "build",
            "pass",
            metrics={
                "preset": preset,
                "fresh_configure": fresh,
                "duration_seconds": time.perf_counter() - start,
                "commands": [command_text(item) for item in commands],
                "discovery": discovery,
            },
            artifacts=[str(build_dir)],
            build=git_metadata(environment),
        ),
        EXIT_PASS,
    )


def command_build(arguments: argparse.Namespace) -> tuple[dict[str, Any], int]:
    return configure_and_build(arguments.preset, arguments.target, fresh=arguments.fresh)


def command_test(arguments: argparse.Namespace) -> tuple[dict[str, Any], int]:
    build_document, build_exit = configure_and_build(arguments.preset)
    if build_exit != EXIT_PASS:
        build_document["command"] = "test"
        return build_document, build_exit
    environment, _ = build_environment()
    cmake = find_cmake()
    assert cmake is not None
    ctest = cmake.with_name("ctest.exe" if os.name == "nt" else "ctest")
    command = [
        str(ctest),
        "--test-dir",
        str(ROOT / "build" / arguments.preset),
        "--output-on-failure",
        "--no-tests=error",
    ]
    if arguments.label:
        command.extend(["--label-regex", arguments.label])
    print(command_text(command), file=sys.stderr)
    start = time.perf_counter()
    process = run_capture(command, environment, timeout=600.0)
    if process.stdout:
        print(process.stdout, file=sys.stderr, end="")
    if process.stderr:
        print(process.stderr, file=sys.stderr, end="")
    passed_match = re.search(r"(\d+)% tests passed, (\d+) tests failed out of (\d+)", process.stdout)
    metrics: dict[str, Any] = {
        "preset": arguments.preset,
        "duration_seconds": time.perf_counter() - start,
        "command": command_text(command),
    }
    if passed_match:
        metrics.update(
            {
                "pass_percent": int(passed_match.group(1)),
                "failed": int(passed_match.group(2)),
                "total": int(passed_match.group(3)),
                "passed": int(passed_match.group(3)) - int(passed_match.group(2)),
            }
        )
    if process.returncode != 0:
        return (
            envelope(
                "test",
                "fail",
                diagnostics=[
                    diagnostic(
                        "INTERNAL_ERROR",
                        "error",
                        "test",
                        "CTest reported failure",
                        context={"exit_code": process.returncode, "preset": arguments.preset},
                    )
                ],
                metrics=metrics,
            ),
            EXIT_FAIL,
        )
    return envelope("test", "pass", metrics=metrics, build=git_metadata(environment)), EXIT_PASS


def runtime_binary(preset: str) -> Path:
    suffix = ".exe" if os.name == "nt" else ""
    return ROOT / "build" / preset / "bin" / f"ai2d_cli{suffix}"


def input_schema_version(path: Path) -> str | None:
    try:
        if path.stat().st_size > 4 * 1024 * 1024:
            return None
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return None
    if not isinstance(document, dict):
        return None
    value = document.get("schema_version")
    return value if isinstance(value, str) else None


def is_game_schema(path: Path) -> bool:
    return input_schema_version(path) in {"0.2", "0.3"}


def forward_runtime(
    command_name: str, arguments: argparse.Namespace, runtime_arguments: list[str]
) -> tuple[dict[str, Any], int]:
    binary = runtime_binary(arguments.preset)
    if not binary.is_file():
        return (
            envelope(
                command_name,
                "unavailable",
                diagnostics=[
                    diagnostic(
                        "COMMAND_UNAVAILABLE",
                        "error",
                        "runtime",
                        f"Runtime command binary is not built for preset '{arguments.preset}'",
                        context={"binary": str(binary)},
                        suggestions=[f"Run 'python tools/engine.py build --preset {arguments.preset} --json'."],
                    )
                ],
            ),
            EXIT_UNAVAILABLE,
        )
    environment, _ = build_environment()
    command = [str(binary), command_name, *runtime_arguments]
    if arguments.json:
        command.append("--json")
    print(command_text(command), file=sys.stderr)
    process = run_capture(command, environment, timeout=1800.0)
    if process.stderr:
        print(process.stderr, file=sys.stderr, end="")
    if arguments.json:
        try:
            document = strict_json_loads(process.stdout)
        except (json.JSONDecodeError, ValueError) as error:
            return (
                envelope(
                    command_name,
                    "fail",
                    diagnostics=[
                        diagnostic(
                            "INTERNAL_ERROR",
                            "error",
                            "runtime",
                            "Runtime emitted invalid JSON",
                            context={"error": str(error), "stdout": process.stdout[:1000]},
                        )
                    ],
                ),
                EXIT_INTERNAL,
            )
        return document, process.returncode
    if process.stdout:
        print(process.stdout, end="")
    return envelope(command_name, "pass" if process.returncode == 0 else "fail"), process.returncode


def command_run(arguments: argparse.Namespace) -> tuple[dict[str, Any], int]:
    if is_game_schema(arguments.scenario):
        runtime_arguments = ["run", str(arguments.scenario), "--frames", str(arguments.frames)]
        for enabled, option in (
            (arguments.headless, "--headless"),
            (arguments.offscreen, "--offscreen"),
            (arguments.hidden, "--hidden"),
            (arguments.no_audio, "--no-audio"),
            (arguments.no_saved_settings, "--no-saved-settings"),
            (arguments.validation, "--validation"),
            (arguments.sync_validation, "--sync-validation"),
        ):
            if enabled:
                runtime_arguments.append(option)
        if arguments.fps is not None:
            runtime_arguments.extend(("--fps", arguments.fps))
        if arguments.input_script is not None:
            runtime_arguments.extend(("--input-script", str(arguments.input_script)))
        return forward_runtime("game", arguments, runtime_arguments)
    if arguments.input_script is not None:
        return (
            envelope(
                "run",
                "fail",
                diagnostics=[
                    diagnostic(
                        "INPUT_INVALID",
                        "error",
                        "cli",
                        "--input-script is supported only for GameManifest 0.2 or 0.3",
                        context={"input": str(arguments.scenario)},
                    )
                ],
            ),
            EXIT_INVALID,
        )
    runtime_arguments = [str(arguments.scenario), "--frames", str(arguments.frames)]
    if arguments.headless:
        runtime_arguments.append("--headless")
    return forward_runtime("run", arguments, runtime_arguments)


def command_validate(arguments: argparse.Namespace) -> tuple[dict[str, Any], int]:
    if is_game_schema(arguments.scenario):
        return forward_runtime("game", arguments, ["validate", str(arguments.scenario)])
    return forward_runtime("validate", arguments, [str(arguments.scenario)])


def command_inspect(arguments: argparse.Namespace) -> tuple[dict[str, Any], int]:
    if arguments.input is not None and is_game_schema(arguments.input):
        if arguments.kind not in ("plan", "scenario"):
            return (
                envelope(
                    "inspect",
                    "fail",
                    diagnostics=[diagnostic("INPUT_INVALID", "error", "cli", "GameManifest supports plan or scenario inspection")],
                ),
                EXIT_INVALID,
            )
        return forward_runtime("game", arguments, ["inspect", str(arguments.input)])
    runtime_arguments = [arguments.kind]
    if arguments.input is not None:
        runtime_arguments.append(str(arguments.input))
    return forward_runtime("inspect", arguments, runtime_arguments)


class PackageAssetValidationError(ValueError):
    def __init__(self, message: str, context: dict[str, Any] | None = None) -> None:
        super().__init__(message)
        self.context = context or {}


class PackagePublicationError(RuntimeError):
    def __init__(
        self,
        error: OSError,
        rollback_errors: list[str],
        backup_workspace: Path | None,
    ) -> None:
        super().__init__(str(error))
        self.rollback_errors = rollback_errors
        self.backup_workspace = backup_workspace


def png_decoded_bytes(path: Path) -> int:
    size = path.stat().st_size
    if size < 24 or size > MAXIMUM_ASSET_SOURCE_BYTES:
        raise PackageAssetValidationError(
            "PNG file size is outside the supported limit",
            {"path": str(path), "bytes": size},
        )
    with path.open("rb") as stream:
        header = stream.read(24)
    signature = b"\x89PNG\r\n\x1a\n"
    if len(header) != 24 or header[:8] != signature or header[8:12] != b"\x00\x00\x00\r" or header[12:16] != b"IHDR":
        raise PackageAssetValidationError("PNG signature or IHDR chunk is invalid", {"path": str(path)})
    width, height = struct.unpack(">II", header[16:24])
    decoded = width * height * 4
    if width == 0 or height == 0 or decoded > MAXIMUM_ASSET_DECODED_BYTES:
        raise PackageAssetValidationError(
            "PNG dimensions exceed the decoded-image limit",
            {"path": str(path), "width": width, "height": height, "decoded_bytes": decoded},
        )
    return decoded


def wav_decoded_bytes(path: Path) -> int:
    size = path.stat().st_size
    if size < 12 or size > MAXIMUM_ASSET_SOURCE_BYTES:
        raise PackageAssetValidationError(
            "WAV file size is outside the supported limit",
            {"path": str(path), "bytes": size},
        )
    with path.open("rb") as stream:
        riff = stream.read(12)
        if len(riff) != 12 or riff[:4] != b"RIFF" or riff[8:12] != b"WAVE":
            raise PackageAssetValidationError("WAV RIFF header is invalid", {"path": str(path)})
        declared_riff_size = struct.unpack("<I", riff[4:8])[0] + 8
        if declared_riff_size > size or declared_riff_size < 12:
            raise PackageAssetValidationError("WAV RIFF size exceeds the file boundary", {"path": str(path)})

        found_format = False
        found_data = False
        audio_format = 0
        channels = 0
        sample_rate = 0
        byte_rate = 0
        block_alignment = 0
        bits_per_sample = 0
        data_size = 0
        offset = 12
        while offset + 8 <= size:
            stream.seek(offset)
            chunk = stream.read(8)
            if len(chunk) != 8:
                raise PackageAssetValidationError("WAV chunk header could not be read", {"path": str(path)})
            chunk_size = struct.unpack("<I", chunk[4:8])[0]
            payload = offset + 8
            if payload + chunk_size > size:
                raise PackageAssetValidationError("WAV chunk exceeds the file boundary", {"path": str(path)})
            if chunk[:4] == b"fmt ":
                if found_format or chunk_size < 16:
                    raise PackageAssetValidationError("WAV format chunk is invalid", {"path": str(path)})
                stream.seek(payload)
                description = stream.read(16)
                if len(description) != 16:
                    raise PackageAssetValidationError("WAV format chunk could not be read", {"path": str(path)})
                (
                    audio_format,
                    channels,
                    sample_rate,
                    byte_rate,
                    block_alignment,
                    bits_per_sample,
                ) = struct.unpack("<HHIIHH", description)
                found_format = True
            elif chunk[:4] == b"data":
                if found_data:
                    raise PackageAssetValidationError("WAV contains multiple data chunks", {"path": str(path)})
                data_size = chunk_size
                found_data = True
            offset = payload + chunk_size + (chunk_size & 1)

    supported_format = audio_format in {1, 3}
    supported_depth = (
        bits_per_sample in {8, 16, 24, 32}
        if audio_format == 1
        else bits_per_sample in {32, 64}
    )
    expected_alignment = channels * ((bits_per_sample + 7) // 8)
    expected_byte_rate = sample_rate * expected_alignment
    if (
        not found_format
        or not found_data
        or not supported_format
        or channels == 0
        or channels > 8
        or sample_rate < 8_000
        or sample_rate > 384_000
        or block_alignment == 0
        or bits_per_sample == 0
        or not supported_depth
        or expected_alignment != block_alignment
        or expected_byte_rate != byte_rate
        or data_size == 0
        or data_size > MAXIMUM_ASSET_SOURCE_BYTES
        or data_size % block_alignment != 0
    ):
        raise PackageAssetValidationError("WAV format or sample data is unsupported", {"path": str(path)})

    source_frames = data_size // block_alignment
    converted_frames = (source_frames * 48_000 + sample_rate - 1) // sample_rate
    decoded = (converted_frames + 4_096) * 2 * 4
    if decoded == 0 or decoded > MAXIMUM_ASSET_DECODED_BYTES:
        raise PackageAssetValidationError(
            "WAV converted PCM exceeds the decoded-audio limit",
            {"path": str(path), "decoded_bytes": decoded},
        )
    return decoded


def package_decoded_asset_bytes(
    assets: object,
    content_root: Path,
    dependency_sources: set[Path],
) -> int:
    if not isinstance(assets, list) or not assets:
        raise PackageAssetValidationError("Game inspection did not provide a non-empty asset list")
    aggregate = 0
    for index, asset in enumerate(assets):
        if not isinstance(asset, dict):
            raise PackageAssetValidationError("Game inspection contains an invalid asset record", {"index": index})
        asset_id = asset.get("id")
        kind = asset.get("kind")
        raw_path = asset.get("path")
        if not isinstance(asset_id, str) or not asset_id or kind not in {"png", "font", "wav"} or not isinstance(raw_path, str):
            raise PackageAssetValidationError("Game inspection contains invalid asset metadata", {"index": index})
        supplied = Path(raw_path)
        if not supplied.is_absolute():
            raise PackageAssetValidationError(
                "Inspected asset path must be absolute",
                {"asset": asset_id, "path": raw_path},
            )
        source = supplied.resolve()
        try:
            source.relative_to(content_root)
        except ValueError as error:
            raise PackageAssetValidationError(
                "Inspected asset path escapes the manifest directory",
                {"asset": asset_id, "path": raw_path},
            ) from error
        if source not in dependency_sources:
            raise PackageAssetValidationError(
                "Inspected asset is absent from the package dependency list",
                {"asset": asset_id, "path": raw_path},
            )
        decoded = wav_decoded_bytes(source) if kind == "wav" else png_decoded_bytes(source)
        if aggregate > MAXIMUM_PACKAGE_DECODED_ASSET_BYTES - decoded:
            raise PackageAssetValidationError(
                "Aggregate decoded startup assets exceed the package limit",
                {
                    "asset": asset_id,
                    "decoded_bytes": decoded,
                    "aggregate_before_bytes": aggregate,
                    "maximum_decoded_asset_bytes": MAXIMUM_PACKAGE_DECODED_ASSET_BYTES,
                },
            )
        aggregate += decoded
    return aggregate


def _replace_package_path(source: Path, destination: Path) -> None:
    source.replace(destination)


def publish_package_outputs(
    staging: Path,
    staged_zip: Path | None,
    package_dir: Path,
    zip_path: Path,
) -> None:
    backup_workspace = Path(
        tempfile.mkdtemp(prefix=f".{package_dir.name}-rollback-", dir=package_dir.parent)
    )
    backup_dir = backup_workspace / "directory"
    backup_zip = backup_workspace / "archive.zip"
    previous_dir_moved = False
    previous_zip_moved = False
    published_dir = False
    published_zip = False
    try:
        if package_dir.exists():
            _replace_package_path(package_dir, backup_dir)
            previous_dir_moved = True
        if staged_zip is not None and zip_path.exists():
            _replace_package_path(zip_path, backup_zip)
            previous_zip_moved = True
        _replace_package_path(staging, package_dir)
        published_dir = True
        if staged_zip is not None:
            _replace_package_path(staged_zip, zip_path)
            published_zip = True
    except OSError as error:
        rollback_errors: list[str] = []

        def restore(source: Path, destination: Path, label: str) -> None:
            try:
                _replace_package_path(source, destination)
            except OSError as rollback_error:
                rollback_errors.append(f"{label}: {rollback_error}")

        if published_zip and zip_path.exists():
            restore(zip_path, staged_zip, "remove newly published ZIP")
        if published_dir and package_dir.exists():
            restore(package_dir, staging, "remove newly published directory")
        if previous_zip_moved and backup_zip.exists():
            restore(backup_zip, zip_path, "restore previous ZIP")
        if previous_dir_moved and backup_dir.exists():
            restore(backup_dir, package_dir, "restore previous directory")

        preserved_workspace: Path | None = backup_workspace if rollback_errors else None
        if not rollback_errors:
            shutil.rmtree(backup_workspace, ignore_errors=True)
        raise PackagePublicationError(error, rollback_errors, preserved_workspace) from error
    else:
        # Both replacements are now complete. Failure to clean a hidden backup
        # must not misreport the newly published pair as failed.
        shutil.rmtree(backup_workspace, ignore_errors=True)


def deterministic_zip(source: Path, destination: Path) -> None:
    files = sorted(
        (path for path in source.rglob("*") if path.is_file()),
        key=lambda path: path.relative_to(source.parent).as_posix(),
    )
    with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as package:
        for path in files:
            relative = path.relative_to(source.parent).as_posix()
            info = zipfile.ZipInfo(relative, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            mode = 0o100755 if path.suffix.casefold() == ".exe" else 0o100644
            info.external_attr = mode << 16
            info.flag_bits |= 0x800
            with path.open("rb") as input_stream, package.open(info, "w", force_zip64=True) as output_stream:
                shutil.copyfileobj(input_stream, output_stream, length=1024 * 1024)


def command_package(arguments: argparse.Namespace) -> tuple[dict[str, Any], int]:
    if os.name != "nt":
        return (
            envelope(
                "package",
                "unavailable",
                diagnostics=[
                    diagnostic(
                        "COMMAND_UNAVAILABLE",
                        "error",
                        "package",
                        "The v0.3 package target currently supports Windows x64 only",
                        context={"os": os.name},
                    )
                ],
            ),
            EXIT_UNAVAILABLE,
        )

    manifest = arguments.manifest.resolve()
    if not manifest.is_file() or not is_game_schema(manifest):
        return (
            envelope(
                "package",
                "fail",
                diagnostics=[
                    diagnostic(
                        "INPUT_INVALID",
                        "error",
                        "package",
                        "Package input must be a readable GameManifest 0.2 or 0.3 file",
                        context={"manifest": str(manifest)},
                    )
                ],
            ),
            EXIT_INVALID,
        )

    build_document, build_exit = configure_and_build(arguments.preset)
    if build_exit != EXIT_PASS:
        build_document["command"] = "package"
        return build_document, build_exit

    binary = runtime_binary(arguments.preset)
    player = ROOT / "build" / arguments.preset / "bin" / "ai2d_player.exe"
    if not binary.is_file() or not player.is_file():
        return (
            envelope(
                "package",
                "unavailable",
                diagnostics=[
                    diagnostic(
                        "COMMAND_UNAVAILABLE",
                        "error",
                        "package",
                        "The package preset did not produce ai2d_cli.exe and ai2d_player.exe",
                        context={"cli": str(binary), "player": str(player)},
                    )
                ],
            ),
            EXIT_UNAVAILABLE,
        )

    environment, _ = build_environment()
    inspected = run_capture(
        [str(binary), "game", "inspect", str(manifest), "--json"], environment, timeout=180.0
    )
    if inspected.stderr:
        print(inspected.stderr, file=sys.stderr, end="")
    try:
        inspection = strict_json_loads(inspected.stdout)
    except (json.JSONDecodeError, ValueError) as error:
        return (
            envelope(
                "package",
                "fail",
                diagnostics=[
                    diagnostic(
                        "INTERNAL_ERROR",
                        "error",
                        "package",
                        "Game inspection emitted invalid JSON",
                        context={"error": str(error), "stdout": inspected.stdout[:1000]},
                    )
                ],
            ),
            EXIT_INTERNAL,
        )
    if inspected.returncode != 0 or inspection.get("status") != "pass":
        inspection["command"] = "package"
        return inspection, inspected.returncode or EXIT_FAIL

    metrics = inspection.get("metrics")
    if not isinstance(metrics, dict):
        raise ValueError("Game inspection is missing metrics")
    schema = metrics.get("schema_version")
    application = metrics.get("application")
    dependencies = metrics.get("dependencies")
    inspected_assets = metrics.get("assets")
    windows_reserved_names = {"CON", "PRN", "AUX", "NUL", *(f"COM{index}" for index in range(1, 10)), *(f"LPT{index}" for index in range(1, 10))}
    if schema not in {"0.2", "0.3"} or not isinstance(application, str) or not re.fullmatch(
        r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}", application
    ) or application.split(".", 1)[0].upper() in windows_reserved_names:
        return (
            envelope(
                "package",
                "fail",
                diagnostics=[
                    diagnostic(
                        "INPUT_INVALID",
                        "error",
                        "package",
                        "Inspection returned an unsupported schema or unsafe application name",
                        context={"schema_version": schema, "application": application},
                    )
                ],
            ),
            EXIT_INVALID,
        )
    if not isinstance(dependencies, list) or not dependencies or not all(isinstance(item, str) for item in dependencies):
        return (
            envelope(
                "package",
                "fail",
                diagnostics=[
                    diagnostic(
                        "INTERNAL_ERROR",
                        "error",
                        "package",
                        "Game inspection did not provide a non-empty relative dependency list",
                    )
                ],
            ),
            EXIT_INTERNAL,
        )

    content_root = manifest.parent.resolve()
    resolved_dependencies: list[tuple[Path, Path]] = []
    seen_destinations: set[str] = set()
    manifest_seen = False
    aggregate_dependency_bytes = 0
    for raw in dependencies:
        relative = Path(raw)
        if relative.is_absolute() or not relative.parts or any(part in ("", ".", "..") for part in relative.parts):
            return (
                envelope(
                    "package",
                    "fail",
                    diagnostics=[
                        diagnostic(
                            "ASSET_PATH_INVALID",
                            "error",
                            "package",
                            "Package dependency must be a normalized relative path",
                            context={"dependency": raw},
                        )
                    ],
                ),
                EXIT_INVALID,
            )
        source = (content_root / relative).resolve()
        try:
            source.relative_to(content_root)
        except ValueError:
            return (
                envelope(
                    "package",
                    "fail",
                    diagnostics=[
                        diagnostic(
                            "ASSET_PATH_INVALID",
                            "error",
                            "package",
                            "Package dependency escapes the manifest directory",
                            context={"dependency": raw},
                        )
                    ],
                ),
                EXIT_INVALID,
            )
        if not source.is_file():
            return (
                envelope(
                    "package",
                    "fail",
                    diagnostics=[
                        diagnostic(
                            "ASSET_PATH_INVALID",
                            "error",
                            "package",
                            "Package dependency is missing or is not a regular file",
                            context={"dependency": raw},
                        )
                    ],
                ),
                EXIT_INVALID,
            )
        source_bytes = source.stat().st_size
        if (
            source_bytes > MAXIMUM_ASSET_SOURCE_BYTES
            or aggregate_dependency_bytes > MAXIMUM_PACKAGE_SOURCE_BYTES - source_bytes
        ):
            return (
                envelope(
                    "package",
                    "fail",
                    diagnostics=[
                        diagnostic(
                            "ASSET_VALIDATION_FAILED",
                            "error",
                            "package",
                            "Package dependency exceeds the bounded source or aggregate size",
                            context={
                                "dependency": raw,
                                "bytes": source_bytes,
                                "maximum_dependency_bytes": MAXIMUM_ASSET_SOURCE_BYTES,
                                "maximum_package_content_bytes": MAXIMUM_PACKAGE_SOURCE_BYTES,
                            },
                        )
                    ],
                ),
                EXIT_INVALID,
            )
        aggregate_dependency_bytes += source_bytes
        destination = Path("game.json") if source == manifest else relative
        if source == manifest:
            manifest_seen = True
        normalized = destination.as_posix().casefold()
        if normalized in seen_destinations:
            return (
                envelope(
                    "package",
                    "fail",
                    diagnostics=[
                        diagnostic(
                            "INPUT_INVALID",
                            "error",
                            "package",
                            "Package dependencies contain a duplicate destination",
                            context={"dependency": raw, "destination": destination.as_posix()},
                        )
                    ],
                ),
                EXIT_INVALID,
            )
        seen_destinations.add(normalized)
        resolved_dependencies.append((source, destination))
    if not manifest_seen:
        return (
            envelope(
                "package",
                "fail",
                diagnostics=[
                    diagnostic(
                        "INTERNAL_ERROR",
                        "error",
                        "package",
                        "Inspection dependency list does not include the manifest",
                    )
                ],
            ),
            EXIT_INTERNAL,
        )

    try:
        aggregate_decoded_asset_bytes = package_decoded_asset_bytes(
            inspected_assets,
            content_root,
            {source for source, _ in resolved_dependencies},
        )
    except PackageAssetValidationError as error:
        return (
            envelope(
                "package",
                "fail",
                diagnostics=[
                    diagnostic(
                        "ASSET_VALIDATION_FAILED",
                        "error",
                        "package",
                        str(error),
                        context=error.context,
                    )
                ],
            ),
            EXIT_INVALID,
        )

    output_root = arguments.output.resolve()
    package_name = f"{application}-windows-x64"
    package_dir = output_root / package_name
    zip_path = output_root / f"{package_name}.zip"
    if package_dir.parent != output_root or zip_path.parent != output_root:
        raise ValueError("Resolved package output escaped its requested output directory")
    protected_paths = [ROOT.resolve(), content_root, manifest, player.resolve(), *(source for source, _ in resolved_dependencies)]
    destructive_overlap = any(
        protected == package_dir or protected.is_relative_to(package_dir) or protected == zip_path
        for protected in protected_paths
    )
    if destructive_overlap:
        return (
            envelope(
                "package",
                "fail",
                diagnostics=[
                    diagnostic(
                        "INPUT_INVALID",
                        "error",
                        "package",
                        "Package output overlaps protected source or build inputs",
                        context={"directory": str(package_dir), "zip": str(zip_path)},
                    )
                ],
            ),
            EXIT_INVALID,
        )
    if package_dir.is_symlink() or zip_path.is_symlink():
        return (
            envelope(
                "package",
                "fail",
                diagnostics=[
                    diagnostic(
                        "INPUT_INVALID",
                        "error",
                        "package",
                        "Package outputs may not be symbolic links",
                        context={"directory": str(package_dir), "zip": str(zip_path)},
                    )
                ],
            ),
            EXIT_INVALID,
        )
    conflicts = [path for path in (package_dir, zip_path if arguments.zip else None) if path is not None and path.exists()]
    if conflicts and not arguments.force:
        return (
            envelope(
                "package",
                "fail",
                diagnostics=[
                    diagnostic(
                        "INPUT_INVALID",
                        "error",
                        "package",
                        "Package output already exists; pass --force to replace the exact output",
                        context={"paths": [str(path) for path in conflicts]},
                    )
                ],
            ),
            EXIT_INVALID,
        )

    output_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=f".{package_name}-", dir=output_root) as temporary:
        staging = Path(temporary) / package_name
        content = staging / "content"
        content.mkdir(parents=True)
        shutil.copyfile(player, staging / f"{application}.exe")
        for source, destination in resolved_dependencies:
            target = content / destination
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
        (staging / "README.txt").write_text(
            "AI2D v0.3 portable Windows package\n\n"
            f"Run {application}.exe from this directory.\n"
            "The content directory must remain beside the executable.\n"
            "A Vulkan 1.3-capable GPU, current vendor driver, and system Vulkan loader are required.\n"
            "The package does not install validation layers or the Vulkan SDK.\n",
            encoding="utf-8",
            newline="\n",
        )
        (staging / "THIRD_PARTY_NOTICES.txt").write_text(
            "AI2D runtime third-party notices\n\n"
            "SDL 3 - zlib license - https://github.com/libsdl-org/SDL\n"
            "Vulkan Memory Allocator - MIT license - https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator\n"
            "nlohmann/json - MIT license - https://github.com/nlohmann/json\n"
            "Vulkan headers/loader interfaces - Apache-2.0 and Khronos licenses - https://www.vulkan.org/\n",
            encoding="utf-8",
            newline="\n",
        )

        staged_zip = Path(temporary) / f"{package_name}.zip"
        if arguments.zip:
            deterministic_zip(staging, staged_zip)

        try:
            publish_package_outputs(
                staging,
                staged_zip if arguments.zip else None,
                package_dir,
                zip_path,
            )
        except PackagePublicationError as error:
            publication_message = (
                "Package publication failed; recovery backups were preserved"
                if error.rollback_errors
                else "Package publication failed; previous outputs were rolled back"
            )
            context: dict[str, Any] = {
                "error": str(error),
                "rollback_errors": error.rollback_errors,
                "directory": str(package_dir),
                "zip": str(zip_path),
            }
            if error.backup_workspace is not None:
                context["recovery_backup"] = str(error.backup_workspace)
            return (
                envelope(
                    "package",
                    "fail",
                    diagnostics=[
                        diagnostic(
                            "INTERNAL_ERROR",
                            "error",
                            "package",
                            publication_message,
                            context=context,
                        )
                    ],
                ),
                EXIT_INTERNAL,
            )

    packaged_files = []
    for path in sorted(item for item in package_dir.rglob("*") if item.is_file()):
        packaged_files.append(
            {
                "path": path.relative_to(package_dir).as_posix(),
                "bytes": path.stat().st_size,
                "sha256": sha256(path),
            }
        )
    package_metrics: dict[str, Any] = {
        "schema_version": schema,
        "application": application,
        "package": str(package_dir),
        "dependencies": [destination.as_posix() for _, destination in resolved_dependencies],
        "source_content_bytes": aggregate_dependency_bytes,
        "decoded_asset_bytes": aggregate_decoded_asset_bytes,
        "files": packaged_files,
    }
    artifacts = [str(package_dir)]
    if arguments.zip:
        package_metrics["zip"] = {"path": str(zip_path), "bytes": zip_path.stat().st_size, "sha256": sha256(zip_path)}
        artifacts.append(str(zip_path))
    return envelope("package", "pass", metrics=package_metrics, artifacts=artifacts), EXIT_PASS


def command_benchmark(arguments: argparse.Namespace) -> tuple[dict[str, Any], int]:
    suite_names = ("world", "render", "scenario") if arguments.suite == "all" else (arguments.suite,)
    source = git_metadata(build_environment()[0])
    suite_documents: list[dict[str, Any]] = []
    exit_code = EXIT_PASS
    for suite_name in suite_names:
        runtime_arguments = [
            "--suite",
            suite_name,
            "--counts",
            arguments.counts,
            "--runs",
            str(arguments.runs),
            "--warmup",
            str(arguments.warmup),
            "--frames",
            str(arguments.frames),
        ]
        if arguments.quick:
            runtime_arguments.append("--quick")
        capture_arguments = argparse.Namespace(**vars(arguments))
        capture_arguments.json = True
        document, child_exit = forward_runtime("benchmark", capture_arguments, runtime_arguments)
        document["fingerprint"] = benchmark_fingerprint(document, arguments.preset, source)
        suite_documents.append(document)
        if child_exit != EXIT_PASS:
            exit_code = child_exit if exit_code == EXIT_PASS else min(exit_code, child_exit)

    if len(suite_documents) == 1:
        document = suite_documents[0]
    else:
        diagnostics = [
            item
            for suite_document in suite_documents
            for item in suite_document.get("diagnostics", [])
        ]
        statuses = [str(item.get("status", "fail")) for item in suite_documents]
        document = envelope(
            "benchmark",
            "pass" if all(status == "pass" for status in statuses) else "fail",
            diagnostics=diagnostics,
            metrics={
                "suite": "all",
                "counts": arguments.counts,
                "suites": suite_documents,
            },
            build=source,
            environment=host_environment(),
        )

    if arguments.baseline is not None:
        try:
            baseline = strict_json_loads(arguments.baseline.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError, ValueError) as error:
            document.setdefault("diagnostics", []).append(
                diagnostic(
                    "INPUT_INVALID",
                    "error",
                    "benchmark",
                    "Baseline artifact could not be read as JSON",
                    context={"path": str(arguments.baseline), "error": str(error)},
                )
            )
            document["status"] = "fail"
            exit_code = EXIT_INVALID
        else:
            document, comparison_exit = attach_comparison(document, baseline)
            if comparison_exit != EXIT_PASS:
                exit_code = comparison_exit

    if arguments.output is not None:
        try:
            arguments.output.parent.mkdir(parents=True, exist_ok=True)
            artifacts = document.setdefault("artifacts", [])
            output_text = str(arguments.output.resolve())
            if output_text not in artifacts:
                artifacts.append(output_text)
            arguments.output.write_text(
                json.dumps(document, ensure_ascii=False, separators=(",", ":"), allow_nan=False) + "\n",
                encoding="utf-8",
            )
        except OSError as error:
            document.setdefault("diagnostics", []).append(
                diagnostic(
                    "INPUT_INVALID",
                    "error",
                    "benchmark",
                    "Benchmark artifact output path could not be written",
                    context={"path": str(arguments.output), "error": str(error)},
                )
            )
            document["status"] = "fail"
            exit_code = EXIT_INVALID
    return document, exit_code


def host_environment() -> dict[str, Any]:
    cpu = platform.processor() or os.environ.get("PROCESSOR_IDENTIFIER") or "unknown"
    return {
        "os": platform.platform(),
        "architecture": platform.machine(),
        "cpu": cpu,
        "logical_core_count": os.cpu_count(),
        "physical_core_count": None,
        "physical_core_count_unavailable_reason": "Python standard library does not expose a portable physical-core count",
    }


def benchmark_fingerprint(
    document: dict[str, Any], preset: str, source: dict[str, Any]
) -> dict[str, Any]:
    build = document.get("build", {})
    environment = document.get("environment", {})
    metrics = document.get("metrics", {})
    host = host_environment()
    instrumentation = {
        "preset": preset,
        "optimized": preset in {"benchmark", "release"},
        "vulkan_validation": preset == "validation",
        "synchronization_validation": preset == "validation",
        "address_sanitizer": preset == "asan",
        "undefined_behavior_sanitizer": False,
        "allocation_tracking": True,
    }
    gpu = {
        key: environment.get(key)
        for key in (
            "vendor_id",
            "device_id",
            "device_name",
            "driver_name",
            "driver_info",
            "driver_version",
            "api_version",
        )
    }
    comparison_key = {
        "benchmark_schema_version": document.get("schema_version"),
        "suite": metrics.get("suite"),
        "compiler": build.get("compiler"),
        "standard_library": f"MSVC STL ({build.get('compiler')})" if "MSVC" in str(build.get("compiler")) else "unknown",
        "configuration": build.get("configuration"),
        "os": host["os"],
        "architecture": host["architecture"],
        "cpu": host["cpu"],
        "logical_core_count": host["logical_core_count"],
        "gpu": gpu,
        "target": metrics.get("target", "headless"),
        "resolution_width": metrics.get("resolution_width"),
        "resolution_height": metrics.get("resolution_height"),
        "runs": metrics.get("runs"),
        "warmup_frames": metrics.get("warmup_frames"),
        "measurement_frames": metrics.get("measurement_frames"),
        "instrumentation": instrumentation,
    }
    fingerprint_hash = hashlib.sha256(
        json.dumps(comparison_key, sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8")
    ).hexdigest()
    return {
        "hash": fingerprint_hash,
        "comparison_key": comparison_key,
        "source": source,
        "host": host,
        "gpu": gpu,
        "instrumentation": instrumentation,
        "eligible_for_baseline": preset == "benchmark",
        "baseline_ineligible_reason": None if preset == "benchmark" else "Only the optimized benchmark preset is baseline-eligible",
    }


def command_promote_baseline(arguments: argparse.Namespace) -> tuple[dict[str, Any], int]:
    if re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}", arguments.name) is None:
        return (
            envelope(
                "promote-baseline",
                "fail",
                diagnostics=[
                    diagnostic(
                        "INPUT_INVALID",
                        "error",
                        "benchmark",
                        "Baseline name contains unsupported characters",
                        context={"name": arguments.name},
                    )
                ],
            ),
            EXIT_INVALID,
        )
    try:
        document = strict_json_loads(arguments.artifact.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError, ValueError) as error:
        return (
            envelope(
                "promote-baseline",
                "fail",
                diagnostics=[
                    diagnostic(
                        "INPUT_INVALID",
                        "error",
                        "benchmark",
                        "Candidate artifact could not be read as JSON",
                        context={"path": str(arguments.artifact), "error": str(error)},
                    )
                ],
            ),
            EXIT_INVALID,
        )
    validation_errors = validate_benchmark_document(document)
    eligible = baseline_eligible(document)
    if validation_errors or not eligible:
        return (
            envelope(
                "promote-baseline",
                "fail",
                diagnostics=[
                    diagnostic(
                        "INPUT_INVALID",
                        "error",
                        "benchmark",
                        "Only a passing optimized benchmark artifact can be promoted",
                        context={
                            "status": document.get("status"),
                            "eligible": eligible,
                            "validation_errors": validation_errors,
                        },
                    )
                ],
            ),
            EXIT_INVALID,
        )
    destination = ARTIFACTS / "benchmarks" / "baselines" / f"{arguments.name}.json"
    if destination.exists() and not arguments.replace:
        return (
            envelope(
                "promote-baseline",
                "fail",
                diagnostics=[
                    diagnostic(
                        "INPUT_INVALID",
                        "error",
                        "benchmark",
                        "Named baseline already exists; use --replace for an explicit overwrite",
                        context={"destination": str(destination)},
                    )
                ],
            ),
            EXIT_INVALID,
        )
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(arguments.artifact, destination)
    return (
        envelope(
            "promote-baseline",
            "pass",
            metrics={"name": arguments.name, "sha256": sha256(destination)},
            artifacts=[str(destination.resolve())],
        ),
        EXIT_PASS,
    )


def command_profile(arguments: argparse.Namespace) -> tuple[dict[str, Any], int]:
    runtime_arguments = [str(arguments.scenario), "--frames", str(arguments.frames)]
    if arguments.raw_jsonl:
        runtime_arguments.extend(["--raw-jsonl", str(arguments.raw_jsonl)])
    return forward_runtime("profile", arguments, runtime_arguments)


def add_common_json(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--json", action="store_true", help="Emit exactly one JSON document on stdout")


def create_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="engine", description="game_runtime_dev command spine")
    subparsers = parser.add_subparsers(dest="command", required=True)

    bootstrap = subparsers.add_parser("bootstrap", help="Install the pinned portable Vulkan/Slang toolchain")
    bootstrap.add_argument("--skip-sdk", action="store_true")
    bootstrap.add_argument("--skip-slang", action="store_true")
    add_common_json(bootstrap)
    bootstrap.set_defaults(handler=command_bootstrap)

    doctor = subparsers.add_parser("doctor", help="Inspect build, Vulkan, shader, and host capabilities")
    add_common_json(doctor)
    doctor.set_defaults(handler=command_doctor)

    for name, handler in (("build", command_build), ("test", command_test)):
        child = subparsers.add_parser(name)
        child.add_argument(
            "--preset",
            choices=("dev", "release", "asan", "validation", "benchmark", "gpu-off"),
            default="dev",
        )
        if name == "build":
            child.add_argument("--target")
            child.add_argument("--fresh", action="store_true")
        else:
            child.add_argument("--label")
        add_common_json(child)
        child.set_defaults(handler=handler)

    run_parser = subparsers.add_parser("run")
    run_parser.add_argument("scenario", type=Path)
    run_parser.add_argument("--frames", type=int, default=600)
    run_parser.add_argument("--headless", action="store_true")
    run_parser.add_argument("--offscreen", action="store_true")
    run_parser.add_argument("--hidden", action="store_true")
    run_parser.add_argument("--fps", choices=("60", "120", "144", "240", "unlimited"))
    run_parser.add_argument("--no-audio", action="store_true")
    run_parser.add_argument("--no-saved-settings", action="store_true")
    run_parser.add_argument("--validation", action="store_true")
    run_parser.add_argument("--sync-validation", action="store_true")
    run_parser.add_argument("--input-script", type=Path)
    run_parser.add_argument("--preset", default="dev")
    add_common_json(run_parser)
    run_parser.set_defaults(handler=command_run)

    validate = subparsers.add_parser("validate")
    validate.add_argument("scenario", type=Path)
    validate.add_argument("--preset", default="dev")
    add_common_json(validate)
    validate.set_defaults(handler=command_validate)

    inspect = subparsers.add_parser("inspect")
    inspect.add_argument("kind", choices=("capabilities", "plan", "scenario", "diagnostics-schema"))
    inspect.add_argument("input", nargs="?", type=Path)
    inspect.add_argument("--preset", default="dev")
    add_common_json(inspect)
    inspect.set_defaults(handler=command_inspect)

    package_parser = subparsers.add_parser("package", help="Build a portable Windows game package")
    package_parser.add_argument("manifest", type=Path)
    package_parser.add_argument("--output", type=Path, default=ARTIFACTS / "packages")
    package_parser.add_argument("--zip", action="store_true")
    package_parser.add_argument("--force", action="store_true")
    package_parser.add_argument("--preset", choices=("package",), default="package")
    add_common_json(package_parser)
    package_parser.set_defaults(handler=command_package)

    benchmark = subparsers.add_parser("benchmark")
    benchmark.add_argument("--suite", choices=("world", "render", "scenario", "all"), default="all")
    benchmark.add_argument("--counts", default="1000,10000,50000,100000")
    benchmark.add_argument("--runs", type=int, default=5)
    benchmark.add_argument("--warmup", type=int, default=300)
    benchmark.add_argument("--frames", type=int, default=3000)
    benchmark.add_argument("--quick", action="store_true")
    benchmark.add_argument("--baseline", type=Path)
    benchmark.add_argument("--output", type=Path)
    benchmark.add_argument("--preset", default="benchmark")
    add_common_json(benchmark)
    benchmark.set_defaults(handler=command_benchmark)

    promote = subparsers.add_parser("promote-baseline")
    promote.add_argument("artifact", type=Path)
    promote.add_argument("name")
    promote.add_argument("--replace", action="store_true")
    add_common_json(promote)
    promote.set_defaults(handler=command_promote_baseline)

    profile_parser = subparsers.add_parser("profile")
    profile_parser.add_argument("scenario", type=Path)
    profile_parser.add_argument("--frames", type=int, default=600)
    profile_parser.add_argument("--raw-jsonl", type=Path)
    profile_parser.add_argument("--preset", default="release")
    add_common_json(profile_parser)
    profile_parser.set_defaults(handler=command_profile)
    return parser


def main() -> int:
    parser = create_parser()
    arguments = parser.parse_args()
    try:
        document, exit_code = arguments.handler(arguments)
    except (OSError, subprocess.SubprocessError, ValueError) as error:
        document = envelope(
            arguments.command,
            "fail",
            diagnostics=[
                diagnostic(
                    "INTERNAL_ERROR",
                    "fatal",
                    "tooling",
                    "Unhandled command-spine error",
                    context={"error": str(error)},
                )
            ],
        )
        exit_code = EXIT_INTERNAL
    emit(document, arguments.json)
    return int(exit_code)


if __name__ == "__main__":
    raise SystemExit(main())
