"""Epic Online Services (EOS SDK's Android Java part, com.epicgames.mobile.eossdk.EOSSDK) keeps its login keychain
in SharedPreferences. When it can't write there, createKeychain() calls resetKeychain(), which calls createKeychain()
again, without end: Beyond Sandbox (GitHub #214) logged that pair ~13,500 times, a 1.85 GB launch.log at 100 % CPU,
and never got past its start. The trigger there was the app's missing internal storage (a game on a drive whose name
has a space, fixed in the agent's launcher); this patch is the guard for any other cause: resetKeychain() returns at
once (in-place classes.dex edit, see vivox_audio_route), so a failed keychain stays failed and EOS reports it to the
game instead of looping. Only that one method changes; EOS logins that work never call it."""
from __future__ import annotations

from ...apk.dex import Dex
from ..base import ApkContext, Patch, register

EOSSDK = "Lcom/epicgames/mobile/eossdk/EOSSDK;"
METHOD = "resetKeychain"


def patch_dex(dex: Dex) -> list[str]:
    """Methods of EOSSDK that now return at once; [] if nothing applies (no such method, or a return type that can't
    be returned early)."""
    done = []
    for idx, code in dex.class_methods(EOSSDK):
        if dex.method(idx)[1] == METHOD and dex.return_early(code, dex.return_type(idx)):
            done.append(METHOD)
    return done


class EosKeychainLoop(Patch):
    id = "frame.eos_keychain_loop"
    title = "Epic Online Services: stop the keychain retry loop"
    description = ("Epic Online Services (EOS) keeps its login keychain in the app's settings storage. When it can't "
                   "write there, it resets the keychain and tries again without end (for example Beyond Sandbox: a "
                   "start that never finishes, 100 % CPU, a log of gigabytes). The reset method returns at once "
                   "instead (an in-place edit of classes.dex), so EOS reports the failure to the game once.")
    order = 44
    needs_vr = False

    def applies(self, a):
        return bool((a.extra or {}).get("eos_keychain"))

    def detect(self, a):
        return None  # only needed when the loop shows up (triage eos-keychain-loop); the usual cause is fixed elsewhere

    def apply(self, ctx: ApkContext) -> bool:
        changed = False
        for name in sorted(n for n in ctx.ws.names() if n.startswith("classes") and n.endswith(".dex")):
            data = ctx.ws.read(name)
            if EOSSDK.encode() not in data:
                continue
            dex = Dex(data)
            if patch_dex(dex):
                ctx.ws.put(name, dex.finish())
                ctx.notes.append(f"{name}: EOSSDK.{METHOD} returns at once")
                changed = True
        return changed


register(EosKeychainLoop)
