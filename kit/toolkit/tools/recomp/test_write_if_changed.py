"""write_if_changed must not touch mtime when the bytes are the same.

That is the whole point of it: a regen rewrites all 54 chunks of generated C,
and an mtime bump on an identical chunk costs a full /O2 rebuild of it.
"""
import os
import tempfile
from pathlib import Path
from types import SimpleNamespace

from .output import generate_header, write_summary
from .translator import BatchTranslator, write_if_changed


def test_write_if_changed():
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "chunk.c")

        assert write_if_changed(p, "a") is True, "must create a missing file"
        assert open(p, encoding="utf-8").read() == "a"

        os.utime(p, (0, 0))
        assert write_if_changed(p, "a") is False, "identical must not write"
        assert os.stat(p).st_mtime == 0, "identical must not bump mtime"

        assert write_if_changed(p, "b") is True, "changed must write"
        assert open(p, encoding="utf-8").read() == "b"
        assert os.stat(p).st_mtime != 0


def test_generated_bytes_use_utf8_and_crlf_and_repair_lf_files():
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / "chunk.c"
        p.write_bytes("caf\u00e9\n".encode("utf-8"))
        assert write_if_changed(p, "caf\u00e9\n")
        assert p.read_bytes() == "caf\u00e9\r\n".encode("utf-8")
        os.utime(p, (0, 0))
        assert not write_if_changed(p, "caf\u00e9\n")
        assert not write_if_changed(p, "caf\u00e9\r\n")
        assert p.stat().st_mtime == 0

        batch = object.__new__(BatchTranslator)
        batch.output_dir = d
        batch.translator = SimpleNamespace(
            owned_function_starts=set(),
            lifter=SimpleNamespace(referenced_calls={}, unimplemented={}))
        single = Path(d) / "single.c"
        batch.translate_batch([], output_file=str(single))
        assert b"\r\n" in single.read_bytes()
        assert b"\n" not in single.read_bytes().replace(b"\r\n", b"")
        single.read_bytes().decode("utf-8")

        types = Path(d) / "recomp_types.h"
        types.write_bytes(b"old\n")
        batch.translate_batch_split([], d)
        data = types.read_bytes()
        assert b"\r\n" in data
        assert b"\n" not in data.replace(b"\r\n", b"")
        os.utime(types, (0, 0))
        batch.translate_batch_split([], d)
        assert types.stat().st_mtime == 0

        header = Path(d) / "separate.h"
        generate_header([], str(header), title="Synthetic")
        summary = Path(write_summary({"example": "caf\u00e9"}, d))
        for output in (header, summary):
            data = output.read_bytes()
            assert b"\r\n" in data
            assert b"\n" not in data.replace(b"\r\n", b"")
            data.decode("utf-8")


if __name__ == "__main__":
    test_write_if_changed()
    print("ok")
