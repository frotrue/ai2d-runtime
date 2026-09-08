from __future__ import annotations

import argparse
import hashlib
import json
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path
from types import SimpleNamespace


def file_sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def directory_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    for item in sorted(candidate for candidate in path.rglob("*") if candidate.is_file()):
        digest.update(item.relative_to(path).as_posix().encode("utf-8"))
        digest.update(b"\0")
        digest.update(item.read_bytes())
    return digest.hexdigest()


def png_header(width: int, height: int) -> bytes:
    return b"\x89PNG\r\n\x1a\n" + struct.pack(">I4sII", 13, b"IHDR", width, height)


def pcm_wav(sample_frames: int = 1, sample_rate: int = 48_000) -> bytes:
    channels = 2
    bits_per_sample = 16
    block_alignment = channels * bits_per_sample // 8
    data = bytes(sample_frames * block_alignment)
    byte_rate = sample_rate * block_alignment
    fmt = struct.pack("<HHIIHH", 1, channels, sample_rate, byte_rate, block_alignment, bits_per_sample)
    riff_size = 4 + 8 + len(fmt) + 8 + len(data)
    return b"RIFF" + struct.pack("<I", riff_size) + b"WAVEfmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(data)) + data


def main() -> int:
    source_root = Path(sys.argv[1]).resolve()
    sys.path.insert(0, str(source_root / "tools"))
    import engine  # pylint: disable=import-outside-toplevel

    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        game_root = root / "game"
        (game_root / "assets").mkdir(parents=True)
        manifest = game_root / "game.json"
        manifest.write_text('{"schema_version":"0.5"}\n', encoding="utf-8")
        used_asset = game_root / "assets" / "used.png"
        used_asset.write_bytes(png_header(1, 1))
        (game_root / "assets" / "not-referenced.bin").write_bytes(b"secret")
        (root / "escape.bin").write_bytes(b"escape")
        bin_dir = root / "build" / "package" / "bin"
        bin_dir.mkdir(parents=True)
        (bin_dir / "ai2d_cli.exe").write_bytes(b"fake-cli")
        (bin_dir / "ai2d_player.exe").write_bytes(b"fake-player")

        inspection_metrics: dict[str, object] = {
            "schema_version": "0.5",
            "application": "package_test",
            "dependencies": ["game.json", "assets/used.png"],
            "assets": [{"id": "used", "kind": "png", "path": str(used_asset.resolve())}],
        }

        original_root = engine.ROOT
        original_build = engine.configure_and_build
        original_environment = engine.build_environment
        original_capture = engine.run_capture
        original_forward = engine.forward_runtime
        original_replace = engine._replace_package_path
        original_os = engine.os
        engine.ROOT = root
        engine.configure_and_build = lambda preset: (engine.envelope("build", "pass"), engine.EXIT_PASS)
        engine.build_environment = lambda: ({}, {})

        def inspect(*_args: object, **_kwargs: object) -> subprocess.CompletedProcess[str]:
            document = {
                "schema_version": 1,
                "command": "game",
                "operation": "inspect",
                "status": "pass",
                "build": {},
                "environment": {},
                "diagnostics": [],
                "metrics": inspection_metrics,
                "artifacts": [],
            }
            return subprocess.CompletedProcess([], 0, json.dumps(document), "")

        engine.run_capture = inspect
        try:
            forwarded: dict[str, object] = {}

            def capture_forward(command: str, _arguments: object, runtime_arguments: list[str]) -> tuple[dict[str, object], int]:
                forwarded["command"] = command
                forwarded["arguments"] = runtime_arguments
                return engine.envelope(command, "pass"), engine.EXIT_PASS

            engine.forward_runtime = capture_forward
            script = root / "input.json"
            script.write_text('{"schema_version":"1","events":[]}\n', encoding="utf-8")
            run_arguments = SimpleNamespace(
                scenario=manifest,
                frames=4,
                headless=True,
                offscreen=False,
                hidden=False,
                no_audio=False,
                no_saved_settings=False,
                validation=False,
                sync_validation=False,
                fps=None,
                input_script=script,
                preset="dev",
                json=True,
            )
            _, forwarded_exit = engine.command_run(run_arguments)
            assert forwarded_exit == engine.EXIT_PASS
            assert forwarded["command"] == "game"
            assert forwarded["arguments"] == [
                "run",
                str(manifest),
                "--frames",
                "4",
                "--headless",
                "--input-script",
                str(script),
            ]
            engine.forward_runtime = original_forward

            output = root / "packages"
            arguments = SimpleNamespace(
                manifest=manifest,
                output=output,
                zip=True,
                force=False,
                preset="package",
                json=True,
            )
            if original_os.name != "nt":
                unavailable, unavailable_exit = engine.command_package(arguments)
                assert unavailable_exit == engine.EXIT_UNAVAILABLE
                assert unavailable["diagnostics"][0]["code"] == "COMMAND_UNAVAILABLE"
                assert not output.exists()
            # Build and runtime inspection are already mocked above. Mock only
            # engine's host gate too, leaving pathlib and real file I/O native.
            engine.os = SimpleNamespace(**{**vars(original_os), "name": "nt"})
            document, exit_code = engine.command_package(arguments)
            assert exit_code == engine.EXIT_PASS and document["status"] == "pass", document
            package_dir = output / "package_test-windows-x64"
            archive = output / "package_test-windows-x64.zip"
            assert (package_dir / "package_test.exe").read_bytes() == b"fake-player"
            assert (package_dir / "content" / "game.json").is_file()
            assert (package_dir / "content" / "assets" / "used.png").is_file()
            assert not (package_dir / "content" / "assets" / "not-referenced.bin").exists()
            assert document["metrics"]["decoded_asset_bytes"] == 4
            first_hash = file_sha256(archive)
            with zipfile.ZipFile(archive) as package:
                names = package.namelist()
                assert names == sorted(names)
                assert all(info.date_time == (1980, 1, 1, 0, 0, 0) for info in package.infolist())

            refused, refused_exit = engine.command_package(arguments)
            assert refused_exit == engine.EXIT_INVALID and refused["status"] == "fail"

            arguments.force = True
            repeated, repeated_exit = engine.command_package(arguments)
            assert repeated_exit == engine.EXIT_PASS and repeated["status"] == "pass"
            assert file_sha256(archive) == first_hash

            old_directory_hash = directory_sha256(package_dir)
            old_zip_hash = file_sha256(archive)
            (bin_dir / "ai2d_player.exe").write_bytes(b"replacement-player")

            failed_zip_once = False

            def fail_zip_publication(source: Path, destination: Path) -> None:
                nonlocal failed_zip_once
                if not failed_zip_once and source.name == archive.name and destination == archive:
                    failed_zip_once = True
                    raise OSError("injected ZIP publication failure")
                original_replace(source, destination)

            engine._replace_package_path = fail_zip_publication
            zip_failed, zip_failed_exit = engine.command_package(arguments)
            assert zip_failed_exit == engine.EXIT_INTERNAL and zip_failed["status"] == "fail"
            assert failed_zip_once
            assert directory_sha256(package_dir) == old_directory_hash
            assert file_sha256(archive) == old_zip_hash

            failed_directory_once = False

            def fail_directory_publication(source: Path, destination: Path) -> None:
                nonlocal failed_directory_once
                if not failed_directory_once and source.name == package_dir.name and destination == package_dir:
                    failed_directory_once = True
                    raise OSError("injected directory publication failure")
                original_replace(source, destination)

            engine._replace_package_path = fail_directory_publication
            directory_failed, directory_failed_exit = engine.command_package(arguments)
            assert directory_failed_exit == engine.EXIT_INTERNAL and directory_failed["status"] == "fail"
            assert failed_directory_once
            assert directory_sha256(package_dir) == old_directory_hash
            assert file_sha256(archive) == old_zip_hash
            engine._replace_package_path = original_replace
            (bin_dir / "ai2d_player.exe").write_bytes(b"fake-player")

            inspection_metrics["dependencies"] = ["game.json", "../escape.bin"]
            contained, contained_exit = engine.command_package(arguments)
            assert contained_exit == engine.EXIT_INVALID and contained["status"] == "fail"
            assert package_dir.is_dir() and archive.is_file()

            inspection_metrics["application"] = "CON"
            inspection_metrics["dependencies"] = ["game.json", "assets/used.png"]
            reserved, reserved_exit = engine.command_package(arguments)
            assert reserved_exit == engine.EXIT_INVALID and reserved["status"] == "fail"

            inspection_metrics["application"] = "package_test"
            oversized = game_root / "assets" / "oversized.bin"
            with oversized.open("wb") as stream:
                stream.seek(64 * 1024 * 1024)
                stream.write(b"x")
            inspection_metrics["dependencies"] = ["game.json", "assets/oversized.bin"]
            bounded, bounded_exit = engine.command_package(arguments)
            assert bounded_exit == engine.EXIT_INVALID and bounded["status"] == "fail"
            assert bounded["diagnostics"][0]["code"] == "ASSET_VALIDATION_FAILED"

            aggregate_assets: list[dict[str, str]] = []
            aggregate_dependencies = ["game.json"]
            for index in range(3):
                relative = f"assets/large-{index}.png"
                asset = game_root / relative
                asset.write_bytes(png_header(5_000, 10_000))
                aggregate_dependencies.append(relative)
                aggregate_assets.append({"id": f"large_{index}", "kind": "png", "path": str(asset.resolve())})
            inspection_metrics["dependencies"] = aggregate_dependencies
            inspection_metrics["assets"] = aggregate_assets
            decoded_bounded, decoded_bounded_exit = engine.command_package(arguments)
            assert decoded_bounded_exit == engine.EXIT_INVALID and decoded_bounded["status"] == "fail"
            assert decoded_bounded["diagnostics"][0]["code"] == "ASSET_VALIDATION_FAILED"
            assert "Aggregate decoded startup assets" in decoded_bounded["diagnostics"][0]["message"]

            small_wav = game_root / "assets" / "small.wav"
            small_wav.write_bytes(pcm_wav())
            inspection_metrics["dependencies"] = ["game.json", "assets/small.wav"]
            inspection_metrics["assets"] = [{"id": "small", "kind": "wav", "path": str(small_wav.resolve())}]
            wav_package, wav_package_exit = engine.command_package(arguments)
            assert wav_package_exit == engine.EXIT_PASS and wav_package["status"] == "pass"
            assert wav_package["metrics"]["decoded_asset_bytes"] == (1 + 4_096) * 2 * 4

            overlap_root = root / "overlap-windows-x64"
            (overlap_root / "assets").mkdir(parents=True)
            overlap_manifest = overlap_root / "game.json"
            overlap_manifest.write_text('{"schema_version":"0.3"}\n', encoding="utf-8")
            overlap_asset = overlap_root / "assets" / "used.png"
            overlap_asset.write_bytes(png_header(1, 1))
            inspection_metrics["application"] = "overlap"
            inspection_metrics["dependencies"] = ["game.json", "assets/used.png"]
            inspection_metrics["assets"] = [{"id": "used", "kind": "png", "path": str(overlap_asset.resolve())}]
            arguments.manifest = overlap_manifest
            arguments.output = root
            arguments.zip = False
            overlap, overlap_exit = engine.command_package(arguments)
            assert overlap_exit == engine.EXIT_INVALID and overlap["status"] == "fail"
            assert overlap_manifest.read_text(encoding="utf-8") == '{"schema_version":"0.3"}\n'
        finally:
            engine.os = original_os
            engine.ROOT = original_root
            engine.configure_and_build = original_build
            engine.build_environment = original_environment
            engine.run_capture = original_capture
            engine.forward_runtime = original_forward
            engine._replace_package_path = original_replace
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
