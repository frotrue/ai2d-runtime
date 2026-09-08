from __future__ import annotations

import sys
import subprocess
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import engine


class CompilerDiscoveryTests(unittest.TestCase):
    def discover(self, platform_name: str, available: dict[str, str], cxx: str = "") -> list[str]:
        environment = {"PATH": "tool-path", "CXX": cxx}
        with patch.object(engine.os, "name", platform_name), patch.object(
            engine.shutil, "which", side_effect=lambda name, path: available.get(name)
        ):
            return engine.compiler_command(environment)

    def test_msvc_remains_the_windows_default(self) -> None:
        self.assertEqual(self.discover("nt", {"cl": "MSVC/cl.exe", "c++": "c++"}), ["MSVC/cl.exe"])

    def test_native_compiler_and_fallbacks(self) -> None:
        for name in ("c++", "g++", "clang++"):
            with self.subTest(name=name):
                self.assertEqual(self.discover("posix", {name: f"/bin/{name}"}), [f"/bin/{name}"])
        self.assertEqual(self.discover("posix", {}), [])

    def test_explicit_compiler_preserves_spaces_and_arguments(self) -> None:
        self.assertEqual(
            self.discover("posix", {"/opt/tool chain/clang++": "/opt/tool chain/clang++"},
                          '"/opt/tool chain/clang++" --target=x86_64-linux-gnu'),
            ["/opt/tool chain/clang++", "--target=x86_64-linux-gnu"],
        )

    def test_missing_explicit_compiler_does_not_fall_back(self) -> None:
        self.assertEqual(self.discover("posix", {"c++": "/bin/c++"}, "missing-c++"), [])

    @unittest.skipIf(sys.platform == "win32", "Native Unix compiler diagnostic")
    def test_doctor_reports_native_compiler_version_without_claiming_gpu_support(self) -> None:
        with patch.object(engine, "build_environment", return_value=({"PATH": ""}, {})), \
             patch.object(engine, "compiler_command", return_value=["/bin/c++"]), \
             patch.object(engine, "find_cmake", return_value=Path("/bin/cmake")), \
             patch.object(engine, "find_ninja", return_value=Path("/bin/ninja")), \
             patch.object(engine, "find_slangc", return_value=None), \
             patch.object(engine, "find_spirv_val", return_value=None), \
             patch.object(engine, "find_vulkan_sdk", return_value=None), \
             patch.object(engine.shutil, "which", return_value=None), \
             patch.object(engine, "git_metadata", return_value={}), \
             patch.object(engine, "version_line", return_value="test version") as version:
            document, exit_code = engine.command_doctor(None)
        self.assertEqual(document["metrics"]["tools"]["compiler"], "/bin/c++")
        self.assertEqual(exit_code, engine.EXIT_UNAVAILABLE)
        self.assertFalse(any(item["severity"] == "error" for item in document["diagnostics"]))
        version.assert_any_call(Path("/bin/c++"), ["--version"], {"PATH": ""})


class TestSummaryTests(unittest.TestCase):
    def test_ctest_old_and_new_summaries_keep_machine_readable_counts(self) -> None:
        cases = (
            ("100% tests passed, 0 tests failed out of 146", 0, 0),
            ("100% tests passed out of 146", 0, 0),
            ("99% tests passed, 2 tests failed out of 146", 8, 2),
        )
        for summary, returncode, failed in cases:
            with self.subTest(summary=summary), \
                 patch.object(engine, "configure_and_build", return_value=({}, 0)), \
                 patch.object(engine, "build_environment", return_value=({}, {})), \
                 patch.object(engine, "find_cmake", return_value=Path("cmake")), \
                 patch.object(engine, "git_metadata", return_value={}), \
                 patch.object(engine, "run_capture", return_value=subprocess.CompletedProcess(
                     [], returncode, summary, ""
                 )):
                document, exit_code = engine.command_test(SimpleNamespace(preset="gpu-off", label=None))
            self.assertEqual(document["metrics"]["total"], 146)
            self.assertEqual(document["metrics"]["failed"], failed)
            self.assertEqual(document["metrics"]["passed"], 146 - failed)
            self.assertEqual(exit_code, engine.EXIT_FAIL if failed else engine.EXIT_PASS)


if __name__ == "__main__":
    unittest.main()
