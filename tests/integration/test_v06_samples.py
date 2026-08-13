from __future__ import annotations

import argparse
import json
import shutil
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


def assert_local_refs(document: object, root: dict[str, object]) -> None:
    if isinstance(document, dict):
        reference = document.get("$ref")
        if isinstance(reference, str) and reference.startswith("#/$defs/"):
            assert reference.removeprefix("#/$defs/") in root["$defs"]
        for value in document.values():
            assert_local_refs(value, root)
    elif isinstance(document, list):
        for value in document:
            assert_local_refs(value, root)


def write_json(path: Path, document: object) -> None:
    path.write_text(json.dumps(document, separators=(",", ":")) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("root", type=Path)
    parser.add_argument("--gpu", action="store_true")
    arguments = parser.parse_args()

    sample = arguments.root / "samples" / "content_foundations"
    invoke(arguments.binary, "game", "validate", str(sample / "game.json"))
    inspection = invoke(arguments.binary, "game", "inspect", str(sample / "game.json"))["metrics"]
    assert inspection["schema_version"] == "0.6"
    assert inspection["plan_hash"] == 14943096261959763208
    assert len(inspection["animations"]) == 2
    assert inspection["prefabs"] == ["animated_actor"]
    assert inspection["locales"] == ["en", "alt"]
    assert inspection["input_profiles"] == ["keyboard", "gamepad"]
    assert inspection["save"]["enabled"] is True
    assert inspection["save"]["slots"] == 2
    assert inspection["save"]["state_count"] == 1
    assert inspection["save"]["capacity_bytes"] > 0
    assert inspection["dependencies"] == sorted(inspection["dependencies"])
    assert "prefabs/animated-actor.json" in inspection["dependencies"]
    assert "locales/alt.json" in inspection["dependencies"]

    verified = invoke(
        arguments.binary,
        "game",
        "verify",
        str(sample / "game.json"),
        "--test-script",
        str(sample / "input" / "verify.json"),
        "--repeat",
        "2",
    )["metrics"]
    assert verified["scene"] == "lab"
    assert verified["states"] == {"score": 1, "save_result": 0, "cell_result": 1}
    assert verified["animation_completions"] == 1
    assert verified["tile_writes"] == 1
    assert verified["field_writes"] == 1
    assert verified["particle_emits"] == 4
    assert verified["camera_follow_updates"] == 24
    assert verified["input_profile_switches"] == 1
    assert verified["locale_switches"] == 1

    save_verified = invoke(
        arguments.binary,
        "game",
        "verify",
        str(sample / "game.json"),
        "--test-script",
        str(sample / "input" / "save-roundtrip.json"),
        "--repeat",
        "2",
    )["metrics"]
    assert save_verified["states"]["score"] == 1
    assert save_verified["states"]["save_result"] == 1
    assert save_verified["save_attempts"] == 2
    assert save_verified["save_successes"] == 2
    assert save_verified["save_failures"] == 0
    assert save_verified["save_bytes"] > 0

    run = invoke(
        arguments.binary,
        "game",
        "run",
        str(sample / "game.json"),
        "--headless",
        "--frames",
        "24",
        "--input-script",
        str(sample / "input" / "smoke.json"),
        "--no-audio",
        "--no-saved-settings",
    )["metrics"]
    assert run["animation_completions"] == 1
    assert run["particle_updates"] > 0
    assert run["camera_shake_updates"] == 6

    for schema_name, expected in (
        ("game-manifest-v0.6.schema.json", "0.6"),
        ("scene-v0.6.schema.json", "0.6"),
        ("prefab-v1.schema.json", "1"),
        ("localization-v1.schema.json", "1"),
    ):
        schema = json.loads((arguments.root / "schemas" / schema_name).read_text(encoding="utf-8"))
        assert schema["properties"]["schema_version"]["const"] == expected
        assert_local_refs(schema, schema)

    with tempfile.TemporaryDirectory(prefix="ai2d-v06-invalid-") as temporary:
        fixture = Path(temporary) / "sample"
        shutil.copytree(sample, fixture)
        manifest_path = fixture / "game.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest.pop("input_profiles")
        write_json(manifest_path, manifest)
        missing_profiles = invoke(
            arguments.binary, "game", "validate", str(manifest_path), expect_success=False
        )
        assert missing_profiles["diagnostics"][0]["code"] == "GAME_INPUT_PROFILE_INVALID"

        shutil.rmtree(fixture)
        shutil.copytree(sample, fixture)
        scene_path = fixture / "scenes" / "lab.json"
        scene = json.loads(scene_path.read_text(encoding="utf-8"))
        scene["tile_layers"][0]["cells"] = [1]
        scene["tile_layers"][0].pop("fill")
        write_json(scene_path, scene)
        invalid_tiles = invoke(
            arguments.binary, "game", "validate", str(fixture / "game.json"), expect_success=False
        )
        assert invalid_tiles["diagnostics"][0]["code"] == "GAME_TILE_FIELD_INVALID"

        shutil.rmtree(fixture)
        shutil.copytree(sample, fixture)
        manifest_path = fixture / "game.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["animations"][0]["frames"][0]["duration_ticks"] = 0
        write_json(manifest_path, manifest)
        invalid_animation = invoke(
            arguments.binary, "game", "validate", str(manifest_path), expect_success=False
        )
        assert invalid_animation["diagnostics"][0]["code"] == "GAME_ANIMATION_INVALID"

        shutil.rmtree(fixture)
        shutil.copytree(sample, fixture)
        prefab_path = fixture / "prefabs" / "animated-actor.json"
        prefab = json.loads(prefab_path.read_text(encoding="utf-8"))
        for component in ("transform", "velocity", "sprite", "collider", "animation"):
            prefab.pop(component, None)
        write_json(prefab_path, prefab)
        invalid_prefab = invoke(
            arguments.binary, "game", "validate", str(fixture / "game.json"), expect_success=False
        )
        assert invalid_prefab["diagnostics"][0]["code"] == "GAME_PREFAB_INVALID", invalid_prefab

        shutil.rmtree(fixture)
        shutil.copytree(sample, fixture)
        manifest_path = fixture / "game.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["save"]["states"] = ["missing_state"]
        write_json(manifest_path, manifest)
        invalid_save = invoke(
            arguments.binary, "game", "validate", str(manifest_path), expect_success=False
        )
        assert invalid_save["diagnostics"][0]["code"] == "GAME_SAVE_INVALID"

        shutil.rmtree(fixture)
        shutil.copytree(sample, fixture)
        locale_path = fixture / "locales" / "alt.json"
        locale = json.loads(locale_path.read_text(encoding="utf-8"))
        locale["strings"].pop("help")
        write_json(locale_path, locale)
        invalid_locale = invoke(
            arguments.binary, "game", "validate", str(fixture / "game.json"), expect_success=False
        )
        assert invalid_locale["diagnostics"][0]["code"] == "GAME_LOCALIZATION_INVALID"

        shutil.rmtree(fixture)
        shutil.copytree(sample, fixture)
        scene_path = fixture / "scenes" / "lab.json"
        scene = json.loads(scene_path.read_text(encoding="utf-8"))
        scene["camera"]["target"]["group"] = "missing_group"
        write_json(scene_path, scene)
        invalid_camera = invoke(
            arguments.binary, "game", "validate", str(fixture / "game.json"), expect_success=False
        )
        assert invalid_camera["diagnostics"][0]["code"] == "GAME_PRESENTATION_INVALID"

        shutil.rmtree(fixture)
        shutil.copytree(sample, fixture)
        scene_path = fixture / "scenes" / "lab.json"
        scene = json.loads(scene_path.read_text(encoding="utf-8"))
        scene["particle_emitters"][0]["capacity"] = 0
        write_json(scene_path, scene)
        invalid_particles = invoke(
            arguments.binary, "game", "validate", str(fixture / "game.json"), expect_success=False
        )
        assert invalid_particles["diagnostics"][0]["code"] == "GAME_PRESENTATION_INVALID"

        shutil.rmtree(fixture)
        shutil.copytree(sample, fixture)
        manifest_path = fixture / "game.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        scene_path = fixture / "scenes" / "lab.json"
        scene = json.loads(scene_path.read_text(encoding="utf-8"))
        scene["grids"][0]["columns"] = 256
        scene["grids"][0]["rows"] = 256
        manifest["scenes"] = []
        for index in range(31):
            scene_id = f"bounded_cells_{index}"
            scene["id"] = scene_id
            relative = f"scenes/{scene_id}.json"
            write_json(fixture / relative, scene)
            manifest["scenes"].append({"id": scene_id, "path": relative})
        manifest["start_scene"] = "bounded_cells_0"
        write_json(manifest_path, manifest)
        aggregate_cells = invoke(
            arguments.binary, "game", "validate", str(manifest_path), expect_success=False
        )
        assert aggregate_cells["diagnostics"][0]["code"] == "GAME_TILE_FIELD_INVALID", aggregate_cells

        shutil.rmtree(fixture)
        shutil.copytree(sample, fixture)
        manifest_path = fixture / "game.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        scene_path = fixture / "scenes" / "lab.json"
        scene = json.loads(scene_path.read_text(encoding="utf-8"))
        emitter = scene["particle_emitters"][0]
        scene["particle_emitters"] = []
        for index in range(5):
            copy = dict(emitter)
            copy["id"] = "sparks" if index == 0 else f"sparks_{index}"
            copy["capacity"] = 9_800
            scene["particle_emitters"].append(copy)
        manifest["scenes"] = []
        for index in range(3):
            scene_id = f"bounded_particles_{index}"
            scene["id"] = scene_id
            relative = f"scenes/{scene_id}.json"
            write_json(fixture / relative, scene)
            manifest["scenes"].append({"id": scene_id, "path": relative})
        manifest["start_scene"] = "bounded_particles_0"
        write_json(manifest_path, manifest)
        aggregate_particles = invoke(
            arguments.binary, "game", "validate", str(manifest_path), expect_success=False
        )
        assert aggregate_particles["diagnostics"][0]["code"] == "GAME_PRESENTATION_INVALID", aggregate_particles

    if arguments.gpu:
        rendered = invoke(
            arguments.binary,
            "game",
            "run",
            str(sample / "game.json"),
            "--offscreen",
            "--hidden",
            "--frames",
            "8",
            "--no-audio",
            "--no-saved-settings",
        )["metrics"]
        assert rendered["last_frame"]["sprites"] >= 34
        assert rendered["localized_text_resolutions"] >= 16
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
