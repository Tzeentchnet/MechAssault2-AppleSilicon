"""Make skipped or expected-failing tests fail the focused setup check."""


def pytest_sessionfinish(session, exitstatus):
    reporter = session.config.pluginmanager.get_plugin("terminalreporter")
    if reporter and (reporter.stats.get("skipped") or reporter.stats.get("xfailed")):
        reporter.write_line("Setup check cannot pass with skipped or xfailed tests", red=True)
        session.exitstatus = 1
