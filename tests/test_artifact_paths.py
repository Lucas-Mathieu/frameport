"""The prebuilt artifacts are committed to a public repo: no build may leave a local path in them (native/build.py maps
source and NDK paths with -ffile-prefix-map; a build from a worktree once left the NDK headers' absolute paths in
libvrapi.so's debug info)."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOCAL_PATH = re.compile(rb"/mnt/[a-z]/|/home/[A-Za-z0-9._-]+/|/Users/[A-Za-z0-9._-]+/|[A-Z]:\\Users\\")


def test_no_local_paths_in_artifacts():
    found = {}
    for f in sorted((ROOT / "artifacts").rglob("*")):
        if f.is_file() and f.name != "SHA256SUMS":
            hits = sorted({m.group(0).decode("latin-1") for m in LOCAL_PATH.finditer(f.read_bytes())})
            if hits:
                found[str(f.relative_to(ROOT))] = hits[:3]
    assert not found, f"local paths in committed artifacts (rebuild from the main checkout): {found}"
