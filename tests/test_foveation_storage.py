"""Native regression: preserve const chains, flags and unknown tails during allocation fix."""
import os
import shutil
import subprocess
from pathlib import Path
import pytest

ROOT = Path(__file__).resolve().parents[1]
INC = next(iter((ROOT / "native/.cache").glob("openxr-*")), None)
CC = shutil.which("cc") or shutil.which("clang")
pytestmark = [pytest.mark.native, pytest.mark.skipif(
    not os.environ.get("FRAMEPORT_NATIVE_TESTS") or not INC or not CC,
    reason="needs FRAMEPORT_NATIVE_TESTS=1, cached OpenXR headers and host C compiler")]

def test_const_chain_storage_fix(tmp_path):
    exe = tmp_path / "storage-test"
    subprocess.run([CC, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-missing-field-initializers", "-I", str(INC),
                    "-I", str(ROOT / "native/adapter"),
                    str(ROOT / "tests/fixtures/src/foveation_storage_test.c"),
                    "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
