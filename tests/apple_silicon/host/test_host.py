#!/usr/bin/env python3
"""Exercise the actual host executable with private, synthetic user homes."""

import json
import os
from pathlib import Path
import resource
import select
import shutil
import signal
import stat
import subprocess
import sys
import time
import unittest
import uuid


ROOT = Path(__file__).resolve().parents[3]
HOST = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else None


class HostProcessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not HOST or not HOST.is_file():
            raise RuntimeError("Pass the built native host executable as the first argument")
        cls.root = ROOT / "_work/apple-silicon/host-process-tests" / uuid.uuid4().hex
        cls.root.mkdir(parents=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.root)

    def setUp(self):
        self.work = self.root / self._testMethodName
        self.work.mkdir()
        self.home = self.work / "private-home-token"
        self.home.mkdir()
        self.scratch = self.work / "compiler-scratch"
        self.scratch.mkdir()
        self.env = dict(os.environ, HOME=str(self.home), TMPDIR=str(self.scratch),
                        SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")

    def run_host(self, *args, env=None, timeout=6):
        return subprocess.run([str(HOST), *args], cwd=ROOT, env=env or self.env,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True, timeout=timeout)

    def records(self, text):
        try:
            return [json.loads(line) for line in text.splitlines()]
        except (ValueError, TypeError):
            self.fail("A host diagnostic was not a structured JSON record")

    def assert_private(self, text, *extra):
        for value in (str(self.home), "private-home-token", *extra):
            self.assertTrue(value not in text, "A private path appeared in host diagnostics")

    def log_text(self):
        path = self.home / "Library/Logs/MechAssault2/host.jsonl"
        return path.read_text(encoding="utf-8")

    def start_waiting_host(self):
        process = subprocess.Popen([str(HOST)], cwd=ROOT, env=self.env,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(self.cleanup_process, process)
        fd = process.stderr.fileno()
        os.set_blocking(fd, False)
        output = bytearray()
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline:
            readable, _, _ = select.select([fd], [], [], max(0, deadline - time.monotonic()))
            if not readable:
                break
            chunk = os.read(fd, 65536)
            if not chunk:
                break
            output.extend(chunk)
            if b'"event":"host_ready"' in output:
                os.set_blocking(fd, True)
                return process, output.decode("utf-8")
        os.set_blocking(fd, True)
        self.fail("The waiting host did not become ready")

    @staticmethod
    def cleanup_process(process):
        if process.poll() is None:
            process.terminate()
            try:
                process.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate(timeout=3)
        else:
            for stream in (process.stdout, process.stderr):
                if stream and not stream.closed:
                    stream.close()

    def test_smoke_real_window_and_worker(self):
        started = time.monotonic()
        result = self.run_host("--smoke", "--safe-start")
        self.assertEqual(result.returncode, 0)
        self.assertLess(time.monotonic() - started, 6)
        self.assertEqual(result.stdout, "")
        self.assert_private(result.stderr)
        events = [entry["event"] for entry in self.records(result.stderr)]
        for event in ("host_start", "guest_unavailable", "safe_start_on",
                      "presentation_software", "worker_started", "host_ready",
                      "worker_stop_requested", "worker_joined", "window_closed",
                      "smoke_passed", "host_exit"):
            self.assertIn(event, events)
        self.assertLess(events.index("worker_started"), events.index("worker_stop_requested"))
        self.assertLess(events.index("worker_stop_requested"), events.index("worker_joined"))
        self.assertLess(events.index("worker_joined"), events.index("window_closed"))
        self.assertLess(events.index("window_closed"), events.index("smoke_passed"))
        for directory in ("Application Support", "Caches", "Logs"):
            self.assertTrue((self.home / "Library" / directory / "MechAssault2").is_dir())
        log_path = self.home / "Library/Logs/MechAssault2/host.jsonl"
        self.assertEqual(stat.S_IMODE(log_path.stat().st_mode), 0o600)
        log_text = self.log_text()
        self.assert_private(log_text)
        self.assertEqual(self.records(log_text), self.records(result.stderr))
        self.assertEqual(stat.S_IMODE(
            (self.home / "Library/Application Support/MechAssault2/host.lock").stat().st_mode),
            0o600)

    def test_help_and_bad_arguments_do_not_echo_or_create_paths(self):
        help_result = self.run_host("--help")
        self.assertEqual(help_result.returncode, 0)
        self.assertIn("not implemented", help_result.stdout)
        self.assertNotIn(str(self.home), help_result.stdout + help_result.stderr)
        bad = self.run_host("--unknown=" + str(self.home / "private-option"))
        self.assertEqual(bad.returncode, 2)
        self.assertEqual(self.records(bad.stderr)[0]["event"], "bad_options")
        self.assert_private(bad.stdout + bad.stderr)
        self.assertFalse((self.home / "Library").exists())

    def test_invalid_and_overlong_home_paths_are_rejected(self):
        for value in ("relative/private-home-token", "/",
                      str(self.home / ".." / "other"), "/" + "private-home-token" * 100):
            with self.subTest(case=value[:12]):
                env = dict(self.env, HOME=value)
                result = self.run_host("--smoke", env=env)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(self.records(result.stderr)[0]["event"], "invalid_home")
                self.assert_private(result.stdout + result.stderr, value)
        env = dict(self.env)
        del env["HOME"]
        missing = self.run_host("--smoke", env=env)
        self.assertNotEqual(missing.returncode, 0)
        self.assertEqual(self.records(missing.stderr)[0]["event"], "invalid_home")
        self.assertFalse((self.home / "Library").exists())

    def test_symlinks_are_not_followed_for_paths_lock_or_log(self):
        outside = self.work / "outside"
        outside.mkdir()
        target = outside / "sentinel"
        target.write_text("unchanged", encoding="utf-8")

        alias = self.work / "home-alias"
        alias.symlink_to(self.home, target_is_directory=True)
        result = self.run_host("--smoke", env=dict(self.env, HOME=str(alias)))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.records(result.stderr)[0]["event"], "paths_unavailable")
        self.assert_private(result.stdout + result.stderr, str(alias))

        library = self.home / "Library"
        library.mkdir()
        (library / "Logs").symlink_to(outside, target_is_directory=True)
        result = self.run_host("--smoke")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.records(result.stderr)[0]["event"], "paths_unavailable")
        self.assertFalse((outside / "MechAssault2").exists())
        self.assert_private(result.stdout + result.stderr, str(outside))
        (library / "Logs").unlink()

        support = library / "Application Support/MechAssault2"
        support.mkdir(parents=True, exist_ok=True)
        (support / "host.lock").symlink_to(target)
        result = self.run_host("--smoke")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.records(result.stderr)[0]["event"], "instance_lock_failed")
        self.assertEqual(target.read_text(encoding="utf-8"), "unchanged")
        self.assert_private(result.stdout + result.stderr, str(outside))
        (support / "host.lock").unlink()

        logs = library / "Logs/MechAssault2"
        logs.mkdir(parents=True, exist_ok=True)
        (logs / "host.jsonl").symlink_to(target)
        result = self.run_host("--smoke")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.records(result.stderr)[0]["event"], "log_failed")
        self.assertEqual(target.read_text(encoding="utf-8"), "unchanged")
        self.assert_private(result.stdout + result.stderr, str(outside))

    def test_runtime_driver_error_cannot_echo_private_path(self):
        driver = str(self.home / "private-video-driver")
        result = self.run_host("--smoke", env=dict(self.env, SDL_VIDEODRIVER=driver))
        self.assertNotEqual(result.returncode, 0)
        events = [entry["event"] for entry in self.records(result.stderr)]
        self.assertIn("window_failed", events)
        self.assert_private(result.stdout + result.stderr, driver)
        log_text = self.log_text()
        self.records(log_text)
        self.assert_private(log_text, driver)

    def test_single_instance_contention_signal_shutdown_and_reacquire(self):
        process, first = self.start_waiting_host()
        second = self.run_host("--smoke")
        self.assertEqual(second.returncode, 3)
        second_records = self.records(second.stderr)
        self.assertEqual(second_records[0]["event"], "instance_busy")
        self.assertIn("close it and retry", second_records[0]["message"])
        self.assert_private(first + second.stdout + second.stderr)

        other_home = self.work / "other-private-home"
        other_home.mkdir()
        other_env = dict(self.env, HOME=str(other_home))
        other = self.run_host("--smoke", env=other_env)
        self.assertEqual(other.returncode, 3)
        self.assertEqual(self.records(other.stderr)[0]["event"], "instance_busy")
        self.assert_private(other.stdout + other.stderr, str(other_home))

        process.send_signal(signal.SIGTERM)
        stdout, tail = process.communicate(timeout=6)
        self.assertEqual(process.returncode, 0)
        output = first + tail.decode("utf-8")
        self.assert_private(output + stdout.decode("utf-8"))
        events = [entry["event"] for entry in self.records(output)]
        self.assertIn("safe_start_off", events)
        self.assertIn("signal_stop", events)
        self.assertLess(events.index("worker_joined"), events.index("window_closed"))
        self.assertEqual(self.run_host("--smoke", env=other_env).returncode, 0)
        self.assert_private(self.log_text())

    def test_fatal_diagnostic_is_redacted_and_lock_is_released(self):
        old_limit = resource.getrlimit(resource.RLIMIT_CORE)
        resource.setrlimit(resource.RLIMIT_CORE, (0, old_limit[1]))
        try:
            process, first = self.start_waiting_host()
            process.send_signal(signal.SIGBUS)
            stdout, tail = process.communicate(timeout=6)
            self.assertEqual(process.returncode, -signal.SIGBUS)
            output = first + tail.decode("utf-8")
            self.assert_private(output + stdout.decode("utf-8"))
            fatal = [entry for entry in self.records(output) if entry["event"] == "signal"]
            self.assertEqual(len(fatal), 1)
            self.assertEqual(fatal[0]["signal"], "SIGBUS")
            log_text = self.log_text()
            self.assert_private(log_text)
            self.assertEqual(self.records(log_text)[-1]["signal"], "SIGBUS")
            other_home = self.work / "other-private-home"
            other_home.mkdir()
            self.assertEqual(self.run_host("--smoke",
                env=dict(self.env, HOME=str(other_home))).returncode, 0)
        finally:
            resource.setrlimit(resource.RLIMIT_CORE, old_limit)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], *sys.argv[2:]], verbosity=2)
