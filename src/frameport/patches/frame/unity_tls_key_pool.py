"""Unity's destructor-less pthread keys share one real key (native/tlskeys, GitHub #198; approach by Klownicle)."""
from __future__ import annotations

from ...analysis import elf
from ..base import ApkContext, Patch, Suggestion, register
from . import artifact

HELPER = "libfp_tkeys.so"
RENAMES = {"pthread_key_create": "fp_tkey_create", "pthread_key_delete": "fp_tkey_delete",
           "pthread_getspecific": "fp_tkey_get", "pthread_setspecific": "fp_tkey_set"}


def _unity_major(a) -> int:
    try:
        return int(str((a.extra or {}).get("unity_version") or "0").split(".")[0] or 0)
    except ValueError:
        return 0


def heavy_native_libs(a) -> list[str]:
    """Libraries that take many thread keys of their own next to Unity 6's (Wander: Firebase C++ and Gecko)."""
    return sorted(n for n in a.libs if n.startswith("libFirebaseCpp") or n == "libxul.so")


class UnityTlsKeyPool(Patch):
    id = "frame.unity_tls_key_pool"
    title = "Unity: share thread keys (games that run out of them)"
    description = (
        "Android gives a process 128 thread keys (pthread keys, per-thread storage slots). Unity 6 takes about 40, "
        "and on the Frame the graphics and OpenXR libraries take more than on a Quest, so games with many other "
        "native libraries run out: Wander aborted at start (\"cannot create thread specific key\" in Firebase). "
        "Unity's keys without a clean-up function become slots behind one real key (libfp_tkeys.so); every other "
        "key and every other library keep real ones. Approach by Klownicle (GitHub #198).")
    order = 46
    needs_vr = False

    def applies(self, a):
        return a.engine == "Unity" and "arm64-v8a" in a.abis and "libunity.so" in a.libs

    def detect(self, a):
        heavy = heavy_native_libs(a)
        if self.applies(a) and _unity_major(a) >= 6000 and heavy:
            return Suggestion(False, f"Unity 6 with {', '.join(heavy)}: turn on if the game aborts at start with "
                                     "\"cannot create thread specific key\" (for example Wander).")
        return None

    def apply(self, ctx: ApkContext) -> bool:
        ws = ctx.ws
        name = ws.lib("libunity.so")
        if ws.abi != "arm64-v8a" or not ws.has(name):
            return False
        data = ws.read(name)
        patched, renamed = elf.rename_imports(data, RENAMES)
        if not renamed and not set(RENAMES.values()) & elf.dyn_symbols(patched, defined=False):
            return False  # no thread-key imports at all
        patched = elf.add_needed(patched, HELPER)
        helper = artifact(ws.abi, HELPER)
        if not ws.has(ws.lib(HELPER)) or ws.read_lib(HELPER) != helper:
            ws.put(ws.lib(HELPER), helper)
        if patched != data:
            ws.put(name, patched)
            ctx.notes.append(f"libunity.so: thread keys through {HELPER} ({', '.join(renamed) or 'already renamed'})")
        return True

    def validate(self, ctx: ApkContext):
        ws = ctx.ws
        name = ws.lib("libunity.so")
        if ws.abi != "arm64-v8a" or not ws.has(name):
            return []
        imports = elf.dyn_symbols(ws.read(name), defined=False)
        return [("Thread-key pool present", ws.has(ws.lib(HELPER)) and HELPER in elf.needed(ws.read(name))
                 and not set(RENAMES) & imports, HELPER)]


register(UnityTlsKeyPool)
