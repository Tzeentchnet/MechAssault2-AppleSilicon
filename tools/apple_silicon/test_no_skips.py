"""The setup check must not mistake missing test coverage for a pass."""

import os
from pathlib import Path

import pytest


pytest_plugins = ("pytester",)


@pytest.mark.parametrize("outcome", ("skip", "xfail"))
def test_nonpassing_test_fails_setup_check(pytester, monkeypatch, outcome):
    monkeypatch.setenv("PYTHONPATH", str(Path(__file__).parent), prepend=os.pathsep)
    pytester.makepyfile(f"""
        import pytest

        def test_pass():
            pass

        @pytest.mark.{outcome}(reason="synthetic")
        def test_not_passed():
            assert False
    """)
    result = pytester.runpytest_subprocess("-q", "-p", "no_skips")
    assert result.ret == pytest.ExitCode.TESTS_FAILED
    result.stdout.fnmatch_lines(["*Setup check cannot pass with skipped or xfailed tests*"])


def test_passing_tests_pass_setup_check(pytester, monkeypatch):
    monkeypatch.setenv("PYTHONPATH", str(Path(__file__).parent), prepend=os.pathsep)
    pytester.makepyfile("def test_pass(): pass")
    result = pytester.runpytest_subprocess("-q", "-p", "no_skips")
    assert result.ret == pytest.ExitCode.OK


def test_collection_skip_fails_setup_check(pytester, monkeypatch):
    monkeypatch.setenv("PYTHONPATH", str(Path(__file__).parent), prepend=os.pathsep)
    pytester.makepyfile(
        test_passing="def test_pass(): pass",
        test_skipped="import pytest\npytest.skip('synthetic', allow_module_level=True)",
    )
    result = pytester.runpytest_subprocess("-q", "-p", "no_skips")
    assert result.ret == pytest.ExitCode.TESTS_FAILED
    result.stdout.fnmatch_lines(["*Setup check cannot pass with skipped or xfailed tests*"])
