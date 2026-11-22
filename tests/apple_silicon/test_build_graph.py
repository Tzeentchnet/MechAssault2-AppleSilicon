#!/usr/bin/env python3
"""Configure the real game graph using synthetic, non-game inputs."""

import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import unittest
import uuid


ROOT = Path(__file__).resolve().parents[2]
os.chdir(ROOT)
GAME = ROOT / "kit/source/game"
RUNTIME = ROOT / "kit/source/runtime"
PATCHES = ROOT / "kit/patches"

# These are the existing Windows release's source-level ABI exceptions.
LEGACY_XMM = {
    "recomp_0002.c", "recomp_0034.c", "recomp_0043.c", "recomp_0054.c",
    "recomp_0057.c", "recomp_0059.c", "recomp_0060.c", "recomp_0061.c",
    "recomp_0074.c", "recomp_0076.c", "recomp_0080.c", "recomp_0081.c",
    "recomp_0082.c", "recomp_0083.c", "recomp_0084.c", "recomp_0085.c",
    "recomp_0087.c", "recomp_0088.c", "recomp_0089.c", "recomp_0090.c",
    "recomp_0091.c", "recomp_0092.c", "recomp_0093.c", "recomp_0099.c",
    "recomp_0100.c", "recomp_bringup_0000.c", "recomp_bringup_0001.c",
}
NOINLINE = {"recomp_0057.c", "recomp_0059.c"}


@unittest.skipUnless(sys.platform == "darwin" and platform.machine() in {"arm64", "aarch64"},
                     "requires native macOS arm64")
class BuildGraphTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = Path("_work/apple-silicon/build-graph-tests") / uuid.uuid4().hex
        cls.work.mkdir(parents=True, exist_ok=False)
        cls.scratch = cls.work / "compiler"
        cls.scratch.mkdir()
        cls.env = dict(os.environ, TMPDIR=str(cls.scratch.resolve()))
        cls.sdl_dir = os.environ.get("SDL2_DIR")
        if not cls.sdl_dir:
            sdl = subprocess.check_output(
                ["brew", "--prefix", "sdl2-compat"], text=True).strip()
            cls.sdl_dir = f"{sdl}/lib/cmake/SDL2"
        cls.openssl = os.environ.get("OPENSSL_ROOT_DIR")
        if not cls.openssl:
            cls.openssl = subprocess.check_output(
                ["brew", "--prefix", "openssl@3"], text=True).strip()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.work)

    def configure(self, name, *options, windows=False, game=GAME):
        build = self.work / name
        cmd = ["cmake", "-S", str(game), "-B", str(build), "-G", "Ninja",
               "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
               f"-DXBOXRECOMP_DIR={RUNTIME}"]
        if windows:
            cmd += ["-DCMAKE_SYSTEM_NAME=Windows",
                    "-DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc",
                    "-DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++",
                    "-DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres"]
        else:
            cmd += ["-DCMAKE_OSX_ARCHITECTURES=arm64",
                    f"-DSDL2_DIR={self.sdl_dir}",
                    f"-DOPENSSL_ROOT_DIR={self.openssl}",
                    f"-DCMAKE_PREFIX_PATH={self.sdl_dir};{self.openssl}"]
            if any(option.startswith("-DMA2_GENERATED_DIR=") for option in options):
                cmd += ["-DMA2_SYNTHETIC_ABI_TEMPLATE=ON"]
        proc = subprocess.run(cmd + list(options), env=self.env,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, timeout=180)
        return build, proc

    def assert_configured(self, result):
        self.assertEqual(result.returncode, 0, result.stdout)

    def fixture(self, name):
        gen = self.work / name / "src/recomp/gen"
        gen.mkdir(parents=True)
        patch = json.loads((PATCHES / "manifest.json").read_text())
        fast = json.loads((PATCHES / "fastcode/fastcode.json").read_text())
        names = set(patch["final"]) | set(fast["final"]) | {
            "recomp_types.h", "recomp_funcs.h", "recomp_bringup.h",
            "recomp_dispatch.c", "recomp_stubs_unresolved.c", "recomp_fastpaths.c",
            "recomp_bringup_0000.c", "recomp_bringup_0001.c", "synthetic_extra.c",
            "main.c", "nv2a_pb_replay.c",
        } | LEGACY_XMM
        for item in names:
            (gen / item).write_text("/* synthetic fixture, not game code */\n")
        shutil.copyfile(
            ROOT / "kit/toolkit/templates/runtime/recomp_types.h",
            gen / "recomp_types.h",
        )
        (gen / "nested").mkdir()
        (gen / "nested/not_discovered.c").write_text("/* synthetic */\n")
        (gen / "not_discovered.cpp").write_text("/* synthetic */\n")
        return gen.resolve(), patch, fast

    @staticmethod
    def commands(build):
        return json.loads((build / "compile_commands.json").read_text())

    @staticmethod
    def generated_commands(build, gen):
        return {Path(entry["file"]).name: entry["command"]
                for entry in BuildGraphTests.commands(build)
                if Path(entry["file"]).parent == gen}

    def test_no_generated_mode_builds_an_explicit_host_shell(self):
        build, proc = self.configure("empty")
        self.assert_configured(proc)
        self.assertIn("Generated game sources and their manual hooks are disabled", proc.stdout)
        self.assertIn("no generated guest was configured", proc.stdout)
        sources = {Path(entry["file"]).name for entry in self.commands(build)}
        self.assertIn("pc_draw_distance.c", sources)
        self.assertIn("ma2_bot_brain_compile.c", sources)
        self.assertTrue({"host_main.c", "host_worker.c", "fb_present_sdl.c"} <= sources)
        self.assertTrue({"nv2a_metal.mm", "nv2a_metal_memory.c"} <= sources)
        metal_command = next(entry["command"] for entry in self.commands(build)
                             if entry["file"].endswith("/nv2a_metal.mm"))
        self.assertIn("-fobjc-arc", metal_command)
        self.assertRegex(metal_command, r"-std=(gnu\+\+|c\+\+)17")
        self.assertNotIn("recomp_manual.c", sources)
        self.assertNotIn("launcher.c", sources)
        self.assertNotIn("nv2a_pgraph_d3d11.c", sources)
        run = subprocess.run(["cmake", "--build", str(build), "--target",
                              "mechassault2_recomp", "--parallel", "3"], env=self.env,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, timeout=180)
        self.assertEqual(run.returncode, 0, run.stdout)
        default = subprocess.run(["cmake", "--build", str(build)], env=self.env,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, timeout=180)
        self.assertEqual(default.returncode, 0, default.stdout)
        executable = build / "mechassault2_recomp"
        self.assertEqual(subprocess.check_output(
            ["lipo", "-archs", str(executable)], env=self.env, text=True).strip(), "arm64")
        help_result = subprocess.run([str(executable), "--help"], env=self.env,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     text=True, timeout=5)
        self.assertEqual(help_result.returncode, 0)
        self.assertIn("Guest entry", help_result.stdout)
        self.assertIn("not implemented", help_result.stdout)
        symbols = subprocess.check_output(["nm", "-gU", str(executable)],
                                          env=self.env, text=True)
        self.assertNotRegex(symbols, r"\b_?(recomp_lookup|recomp_lookup_manual|ma2_xact_on_free)\b")

    def test_enabled_sources_are_exact_and_glob_rechecks(self):
        gen, _, _ = self.fixture("apple-fixture")
        build, proc = self.configure("apple-enabled", f"-DMA2_GENERATED_DIR={gen}")
        self.assert_configured(proc)
        commands = self.generated_commands(build, gen)
        self.assertEqual(set(commands), {p.name for p in gen.glob("*.c")})
        for name, command in commands.items():
            self.assertIn("-DRECOMP_XMM_INTRIN=0", command, name)
            self.assertIn("-ivfsoverlay", command, name)
            self.assertEqual("-fno-inline-functions" in command, name in NOINLINE, name)
            self.assertNotIn("/Ob0", command)
        sources = {Path(entry["file"]).name for entry in self.commands(build)}
        self.assertTrue({"recomp_manual.c", "ma2_tank_fixedstep.c", "ma2_bots_ui.c"} <= sources)

        (gen / "synthetic_new.c").write_text("/* synthetic */\n")
        run = subprocess.run(["cmake", "--build", str(build), "--target",
                              "mechassault2_recomp", "--parallel", "3"], env=self.env,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, timeout=180)
        self.assertEqual(run.returncode, 0, run.stdout)
        self.assertIn("synthetic_new.c", self.generated_commands(build, gen))

        (gen / "recomp_bringup.h").unlink()
        run = subprocess.run(["cmake", "--build", str(build), "--target",
                              "mechassault2_recomp"], env=self.env,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("generated input", run.stdout)

    def test_incomplete_inputs_fail_at_configure(self):
        gen, patch, fast = self.fixture("missing-fixture")
        victim = sorted(set(fast["final"]) - set(patch["final"]))[0]
        (gen / victim).unlink()
        _, proc = self.configure("missing-fastcode", f"-DMA2_GENERATED_DIR={gen}")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("FastCode output", proc.stdout)

        (gen / victim).write_text("/* synthetic */\n")
        (gen / "recomp_bringup.h").unlink()
        _, proc = self.configure("missing-manual-header", f"-DMA2_GENERATED_DIR={gen}")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("generated input", proc.stdout)

        (gen / "recomp_bringup.h").write_text("/* synthetic */\n")
        _, proc = self.configure("missing-patches", f"-DMA2_GENERATED_DIR={gen}",
                                 f"-DMA2_PATCH_DIR={(self.work / 'absent').resolve()}")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("patch manifest", proc.stdout)

        patches = self.work / "synthetic-patches"
        patches.mkdir()
        (patches / "manifest.json").write_text(json.dumps({
            "final": {"synthetic_extra.c": "fixture"},
            "patched": ["missing_payload.c"], "added": [],
        }))
        _, proc = self.configure("missing-patch-payload", f"-DMA2_GENERATED_DIR={gen}",
                                 f"-DMA2_PATCH_DIR={patches.resolve()}")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("patch input", proc.stdout)

        (patches / "manifest.json").write_text(json.dumps({
            "final": {"synthetic_extra.c": "fixture"}, "patched": [], "added": [],
        }))
        fast_dir = patches / "fastcode"
        post_dir = fast_dir / "post"
        post_dir.mkdir(parents=True)
        (fast_dir / "fastcode.json").write_text(json.dumps({
            "final": {"synthetic_extra.c": "fixture"}, "hot": "hot.txt", "post": "post",
        }))
        (fast_dir / "fastcode.py").write_text("# synthetic\n")
        (fast_dir / "hot.txt").write_text("# synthetic\n")
        (post_dir / "manifest.json").write_text(json.dumps({
            "final": {"missing_post_output.c": "fixture"}, "patched": [], "added": [],
        }))
        _, proc = self.configure("missing-post-output", f"-DMA2_GENERATED_DIR={gen}",
                                 f"-DMA2_PATCH_DIR={patches.resolve()}")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("post-FastCode patch output", proc.stdout)

        (post_dir / "manifest.json").write_text(json.dumps({
            "final": {"synthetic_extra.c": "fixture"}, "patched": [],
            "added": ["missing_post_payload.c"],
        }))
        _, proc = self.configure("missing-post-payload", f"-DMA2_GENERATED_DIR={gen}",
                                 f"-DMA2_PATCH_DIR={patches.resolve()}")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("patch input", proc.stdout)

    def test_incompatible_options_fail_without_fallback(self):
        _, proc = self.configure("d3d8-on", "-DXBOXRECOMP_BUILD_D3D8=ON")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("requires XBOXRECOMP_BUILD_D3D8=OFF", proc.stdout)
        _, proc = self.configure("pc-assets", "-DMA2_BUILD_PC_ASSETS=ON")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("require the Windows game/launcher graph", proc.stdout)
        _, proc = self.configure("missing-metal",
                                 f"-DMA2_METAL_COMPILER={(self.work / 'absent-metal').resolve()}")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("macOS metal tool does not exist", proc.stdout)

        invalid_tool = self.work / "invalid-metal-tool"
        invalid_tool.write_text("not an executable\n", encoding="utf-8")
        _, proc = self.configure("invalid-metal",
                                 f"-DMA2_METAL_COMPILER={invalid_tool.resolve()}")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("macOS metal tool is not executable", proc.stdout)

        _, proc = self.configure("wrong-architecture", "-DCMAKE_OSX_ARCHITECTURES=x86_64")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("requires exactly CMAKE_OSX_ARCHITECTURES=arm64", proc.stdout)

        invalid_framework = self.work / "invalid-framework"
        invalid_framework.write_text("not a framework\n", encoding="utf-8")
        _, proc = self.configure("invalid-appkit",
                                 f"-DMA2_APPKIT_FRAMEWORK={invalid_framework.resolve()}")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("toolchain cannot link", proc.stdout)

    def test_windows_static_invariants(self):
        cmake = (GAME / "CMakeLists.txt").read_text()
        match = re.search(r"set\(MA2_LEGACY_XMM_NAMES\s+(.*?)\)", cmake, re.S)
        self.assertIsNotNone(match)
        self.assertEqual(set(match.group(1).split()), LEGACY_XMM)
        for token in ("enable_language(RC)", "add_executable(mechassault2_launcher WIN32",
                      "add_executable(${PROJECT_NAME} WIN32", "src/recomp_manual.c",
                      "imgui_impl_win32.cpp", "imgui_impl_dx11.cpp",
                      "IMGUI_IMPL_WIN32_DISABLE_GAMEPAD", "CONFIGURE_DEPENDS",
                      "/Ob0", "/MANIFEST:EMBED", "/MANIFESTINPUT:",
                      "/LARGEADDRESSAWARE", "/MAP", "/bigobj",
                      "avcodec-63", "avformat-63", "avutil-61", "swresample-7",
                      "swscale-10", "ma2_assert_windows_graph()"):
            self.assertIn(token, cmake)

    def test_windows_cross_graph_preserves_release_contract(self):
        tools = ["x86_64-w64-mingw32-gcc", "x86_64-w64-mingw32-g++",
                 "x86_64-w64-mingw32-windres"]
        if not all(shutil.which(tool) for tool in tools):
            self.skipTest("MinGW cross compiler/RC unavailable; use static Windows assertions")
        gen, _, _ = self.fixture("windows-fixture")
        build, proc = self.configure("windows", f"-DMA2_GENERATED_DIR={gen}",
                                     "-DMA2_BUILD_PC_ASSETS=ON", windows=True)
        self.assert_configured(proc)
        commands = self.generated_commands(build, gen)
        self.assertEqual(set(commands), {p.name for p in gen.glob("*.c")})
        for name, command in commands.items():
            self.assertEqual("-DRECOMP_XMM_INTRIN=0" in command, name in LEGACY_XMM, name)
            self.assertEqual("/Ob0" in command, name in NOINLINE, name)
        entries = self.commands(build)
        sources = {Path(entry["file"]).name for entry in entries}
        self.assertTrue({"main.c", "launcher.c", "recomp_manual.c", "bink_native.c",
                         "ma2_scenario.c", "ma2_scenario_audio.cpp",
                         "imgui_impl_win32.cpp", "imgui_impl_dx11.cpp"} <= sources)
        win32_imgui = next(e["command"] for e in entries
                           if Path(e["file"]).name == "imgui_impl_win32.cpp")
        self.assertIn("IMGUI_IMPL_WIN32_DISABLE_GAMEPAD", win32_imgui)
        launcher = next(e["command"] for e in entries
                        if Path(e["file"]).name == "launcher.c")
        self.assertNotRegex(launcher, r"(?:^|\s)-std=")
        graph = (build / "build.ninja").read_text()
        for token in ("app.rc", "MechAssault 2.exe", "-ld3d11", "-ldxgi", "-ldxguid",
                      "-lxinput", "-lwinmm", "-ldbghelp", "avcodec.lib", "avformat.lib",
                      "avutil.lib", "swscale.lib", "avcodec-63.dll", "avformat-63.dll",
                      "avutil-61.dll", "swresample-7.dll", "swscale-10.dll",
                      "FFmpeg-GPL-LICENSE.txt", "ImGui-MIT-LICENSE.txt",
                      "libsamplerate-LICENSE.txt", "DSP-GPL-LICENSE.txt",
                      "runtime-LGPL-LICENSE.txt", "pc_assets", "pc_package"):
            self.assertIn(token, graph)
        cmake = (GAME / "CMakeLists.txt").read_text()
        for flag in ("/MANIFEST:EMBED", "/MANIFESTINPUT:", "/LARGEADDRESSAWARE",
                     "/MAP", "/bigobj"):
            self.assertIn(flag, cmake)

    def test_windows_installer_deferred_chunk_options(self):
        if not all(shutil.which(t) for t in (
                "x86_64-w64-mingw32-gcc", "x86_64-w64-mingw32-g++",
                "x86_64-w64-mingw32-windres")):
            self.skipTest("MinGW cross compiler/RC unavailable")
        gen, _, _ = self.fixture("windows-install-game")
        game = gen.parents[2]
        (game / "CMakeLists.txt").symlink_to(GAME / "CMakeLists.txt")
        (game / "cmake").symlink_to(GAME / "cmake", target_is_directory=True)
        cmake = (GAME / "CMakeLists.txt").read_text()
        sources = re.search(r"set\(GAME_SOURCES\s+(.*?)\)", cmake, re.S).group(1).split()
        for source in sources + ["src/launcher.c"]:
            path = game / source
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("/* synthetic fixture */\n")
        include = self.work / "deferred.cmake"
        include.write_text(
            'function(check_lowmem)\n'
            '  get_property(old SOURCE "src/recomp/gen/recomp_0057.c" PROPERTY COMPILE_OPTIONS)\n'
            '  if(NOT "/Ob0" IN_LIST old)\n'
            '    message(FATAL_ERROR "Original chunk option missing from relative source")\n'
            '  endif()\n'
            '  set_property(SOURCE "src/recomp/gen/recomp_0057.c" PROPERTY COMPILE_OPTIONS /cgthreads1 /O1 ${old})\n'
            'endfunction()\n'
            'cmake_language(DEFER CALL check_lowmem)\n')
        build, proc = self.configure(
            "windows-deferred",
            f"-DCMAKE_PROJECT_mechassault2_recomp_INCLUDE={include.resolve()}",
            windows=True, game=game)
        self.assert_configured(proc)
        command = self.generated_commands(build, gen)["recomp_0057.c"]
        self.assertRegex(command, r"/cgthreads1\s+/O1\s+/Ob0")


if __name__ == "__main__":
    unittest.main(verbosity=2)
