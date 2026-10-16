"""Native regression: dispatcher filtering and reversible Vulkan output-chain stripping."""
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

def test_foveation_dispatcher_and_images(tmp_path):
    exe = tmp_path / "storage-test"
    subprocess.run([CC, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-missing-field-initializers", "-I", str(INC),
                    "-I", str(ROOT / "native/adapter"),
                    str(ROOT / "tests/fixtures/src/hide_fb_foveation_test.c"),
                    "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


def test_shim_filters_synthetic_dispatcher_before_instance(tmp_path):
    android = tmp_path / "android"
    android.mkdir()
    (android / "log.h").write_text(
        "#define ANDROID_LOG_INFO 4\nint __android_log_print(int, const char *, const char *, ...);\n")
    exe = tmp_path / "dispatcher-test"
    subprocess.run([CC, "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                    "-Wno-missing-field-initializers", "-I", str(INC), "-I", str(tmp_path),
                    "-I", str(ROOT / "native/xrshim"),
                    str(ROOT / "tests/fixtures/src/hide_fb_dispatcher_test.c"),
                    "-pthread", "-lm", "-ldl", "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
