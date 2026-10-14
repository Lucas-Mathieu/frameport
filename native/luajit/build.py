"""Build the pinned LuaJIT engine; the game's xLua bindings are retained, never bundled here."""
from __future__ import annotations

import argparse
import hashlib
import shlex
import shutil
import subprocess
import tarfile
import urllib.request
from pathlib import Path

REVISION = "634daf2453559fd5e0ec282f3b7aedaa09e9f344"
ARCHIVE_SHA256 = "a66bc46efe5417c949fe08e224fd5362a5d395c7d96ff5acbd428580fc6b3448"
HERE = Path(__file__).resolve().parent
CACHE = HERE.parent / ".cache" / "luajit"


def sources() -> Path:
    CACHE.mkdir(parents=True, exist_ok=True)
    archive = CACHE / f"{REVISION}.tar.gz"
    if not archive.exists():
        url = f"https://codeload.github.com/LuaJIT/LuaJIT/tar.gz/{REVISION}"
        with urllib.request.urlopen(url) as response:
            archive.write_bytes(response.read())
    if hashlib.sha256(archive.read_bytes()).hexdigest() != ARCHIVE_SHA256:
        raise RuntimeError("LuaJIT source archive checksum mismatch")
    root = CACHE / f"LuaJIT-{REVISION}"
    if not root.exists():
        with tarfile.open(archive) as tar:
            tar.extractall(CACHE, filter="data")
    return root / "src"


def build(tc: Path, output: Path) -> None:
    src = sources()
    clang = tc / "aarch64-linux-android21-clang"
    flags = "-fPIC -DLUAJIT_UNWIND_INTERNAL " + shlex.quote(f"-ffile-prefix-map={src}=LuaJIT/src")
    # Quotes are part of Make's variable value: the toolchain can live in a path with spaces.
    settings = ["HOST_CC=cc", f"TARGET_CC={shlex.quote(str(clang))}",
                f"TARGET_LD={shlex.quote(str(clang))}",
                f"TARGET_AR={shlex.quote(str(tc / 'llvm-ar'))} rcus",
                f"TARGET_STRIP={shlex.quote(str(tc / 'llvm-strip'))}",
                "TARGET_SYS=Linux", f"TARGET_FLAGS={flags}", "BUILDMODE=static"]
    subprocess.run(["make", "-C", str(src), "clean", *settings], check=True)
    subprocess.run(["make", "-C", str(src), "-j4", "libluajit.a", *settings], check=True)
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(clang), "-shared", "-fPIC", "-O2",
                    "-DLUAJIT_UNWIND_INTERNAL", "-I", str(src), "runtime.c", "-Wl,--whole-archive",
                    str(src / "libluajit.a"), "-Wl,--no-whole-archive", "-lm", "-ldl", "-llog",
                    "-Wl,-soname,libfp_luajit.so", "-Wl,-z,max-page-size=16384", "-o", str(output)],
                   cwd=HERE, check=True)
    notices = output.parent.parent / "licenses"
    notices.mkdir(exist_ok=True)
    shutil.copyfile(HERE / "COPYRIGHT", notices / "LuaJIT.txt")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--clang-dir", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    build(args.clang_dir.resolve(), args.out.resolve())
