"""Save backups of Quest games (GitHub discussion #352: Steam doesn't sync them).

The Frame agent streams a game's saves as one tar.gz over an SSH exec channel (`_backup_saves`: its internal
/data/data/<pkg> folder and its /sdcard/Android/data/<pkg> folder, caches and big content left out; the archive's
last member `frameport-saves.json` lists every file with size and sha256). Backups live on this PC in
<user data>/save-backups/<pkg>/<YYYYmmdd-HHMMSS>.tar.gz with a <same>.json sidecar (the manifest without the file
list); the newest `keep_count()` per game are kept (setting `saves.keep`). Restoring streams one back
(`_restore_saves`): the agent checks the whole archive first (paths, links, sizes, hashes), keeps the Frame's current
saves as <base>/saves-before-restore.tar.gz (one copy: `undo_restore`), then swaps the files in. Both refuse while the
game runs. No Flet here: the GUI and the CLI share it.
"""
from __future__ import annotations

import json
import re
import tarfile
import time
from dataclasses import dataclass
from pathlib import Path

from ..core import library
from ..core.events import Reporter
from ..core.paths import user_data_dir

KEEP_DEFAULT = 10
KEEP_CHOICES = (3, 5, 10, 20, 50)
MANIFEST = "frameport-saves.json"
_NAME = re.compile(r"^\d{8}-\d{6}(-\d+)?\.tar\.gz$")
_PKG = re.compile(r"^[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z0-9_]+)+$")
CHUNK = 1 << 16
EXEC_TIMEOUT = 600  # s without data from the Frame (a dead link must not hang a job)


class SavesError(RuntimeError):
    """The Frame refused or failed (its own sentence, e.g. "the game is running")."""


@dataclass
class Backup:
    package: str
    path: Path
    created: float
    size: int        # the archive on this PC
    files: int = 0
    bytes: int = 0   # the save files' size (uncompressed)
    version: str = ""
    title: str = ""
    skipped: int = 0  # large files left out

    @property
    def sidecar(self) -> Path:
        return sidecar_of(self.path)


def root_dir() -> Path:
    return user_data_dir() / "save-backups"


def backups_dir(pkg: str) -> Path:
    if not _PKG.match(pkg):
        raise ValueError(f"bad package name {pkg!r}")
    return root_dir() / pkg


def sidecar_of(archive: Path) -> Path:
    return archive.with_name(archive.name[: -len(".tar.gz")] + ".json")


def keep_count() -> int:
    try:
        n = int(library.setting("saves.keep", KEEP_DEFAULT))
    except (TypeError, ValueError):
        n = KEEP_DEFAULT
    return max(1, n)


def read_manifest(archive: Path) -> dict:
    """The manifest inside a backup (reads the whole archive: also checks it's complete). ValueError if it isn't a
    FramePort save backup."""
    try:
        with tarfile.open(archive, "r:gz") as tar:
            found = None
            for m in tar:
                if m.name == MANIFEST and m.isreg():
                    found = json.loads(tar.extractfile(m).read().decode("utf-8"))
    except (OSError, tarfile.TarError, EOFError, ValueError) as exc:
        raise ValueError(f"{archive.name} isn't a readable save backup ({exc})") from None
    if not isinstance(found, dict) or found.get("format") != 1 or not found.get("package"):
        raise ValueError(f"{archive.name} isn't a FramePort save backup")
    return found


def _sidecar_data(manifest: dict, archive: Path) -> dict:
    files = manifest.get("files") or []
    return {"package": manifest.get("package"), "created": manifest.get("created"),
            "version": manifest.get("version") or "", "title": manifest.get("title") or "",
            "files": len(files), "bytes": sum(int(f.get("size") or 0) for f in files),
            "skipped": [s for s in manifest.get("skipped") or [] if s.get("reason") in ("size", "unreadable")],
            "archive": archive.name, "agent": manifest.get("agent")}


def _from_sidecar(archive: Path, data: dict) -> Backup:
    return Backup(package=str(data.get("package") or archive.parent.name), path=archive,
                  created=float(data.get("created") or archive.stat().st_mtime), size=archive.stat().st_size,
                  files=int(data.get("files") or 0), bytes=int(data.get("bytes") or 0),
                  version=str(data.get("version") or ""), title=str(data.get("title") or ""),
                  skipped=len(data.get("skipped") or []))


def list_backups(pkg: str) -> list[Backup]:
    """This game's backups on this PC, newest first (a missing/broken sidecar is rebuilt from the archive)."""
    d = backups_dir(pkg)
    out = []
    for archive in sorted(d.glob("*.tar.gz")) if d.is_dir() else []:
        if not _NAME.match(archive.name):
            continue
        try:
            data = json.loads(sidecar_of(archive).read_text(encoding="utf-8"))
        except (OSError, ValueError):
            try:
                data = _sidecar_data(read_manifest(archive), archive)
                sidecar_of(archive).write_text(json.dumps(data, indent=1), encoding="utf-8")
            except (OSError, ValueError):
                continue
        try:
            out.append(_from_sidecar(archive, data))
        except OSError:
            continue
    return sorted(out, key=lambda b: b.created, reverse=True)


def has_backups(pkg: str) -> bool:
    try:
        d = backups_dir(pkg)
    except ValueError:
        return False
    return d.is_dir() and any(_NAME.match(p.name) for p in d.glob("*.tar.gz"))


def prune(pkg: str, keep: int | None = None, protect: Path | None = None) -> list[Path]:
    """Delete all but the newest `keep` backups of a game (never `protect`). Returns the deleted archives."""
    keep = keep_count() if keep is None else max(1, keep)
    gone = []
    for b in list_backups(pkg)[keep:]:
        if protect is not None and b.path == protect:
            continue
        for p in (b.path, b.sidecar):
            p.unlink(missing_ok=True)
        gone.append(b.path)
    return gone


def delete(backup: Backup) -> None:
    backup.path.unlink(missing_ok=True)
    backup.sidecar.unlink(missing_ok=True)


# ------------------------------------------------------------------------------------------ the Frame side
def _quote(s: str) -> str:
    return "'" + s.replace("'", "'\"'\"'") + "'"


def _exec(frame, command: str, args: dict):
    remote = frame.ensure_agent()
    return frame.client.exec_command(f"python3 {_quote(remote)} {command} {_quote(json.dumps(args))}",
                                     timeout=EXEC_TIMEOUT)


def _last_json(text: str) -> dict | None:
    for line in reversed(text.splitlines()):
        line = line.strip()
        if line.startswith("{"):
            try:
                return json.loads(line)
            except ValueError:
                return None
    return None


def info(frame, pkg: str) -> dict:
    """{files, bytes, skipped, running, version, rollback: {size, time} | None} of the game's saves on the Frame."""
    return frame.agent("saves_info", package=pkg)


def _new_name(d: Path) -> Path:
    stamp = time.strftime("%Y%m%d-%H%M%S")
    path, n = d / f"{stamp}.tar.gz", 1
    while path.exists() or path.with_name(path.name + ".part").exists():
        path, n = d / f"{stamp}-{n}.tar.gz", n + 1
    return path


def backup(frame, pkg: str, reporter: Reporter | None = None, keep: int | None = None) -> Backup:
    """Back up the game's saves from the Frame into a new archive on this PC, then keep the newest `keep`."""
    rep = reporter or Reporter()
    rep.stage("saves", "Backing up saves")
    try:
        expected = int(info(frame, pkg).get("bytes") or 0)
    except Exception:  # noqa: BLE001 - only for the progress bar
        expected = 0
    d = backups_dir(pkg)
    d.mkdir(parents=True, exist_ok=True)
    dest = _new_name(d)
    part = dest.with_name(dest.name + ".part")
    stdin, stdout, stderr = _exec(frame, "_backup_saves", {"package": pkg})
    received = 0
    try:
        stdin.channel.shutdown_write()
        with open(part, "wb") as f:
            while True:
                rep.check_cancel()
                chunk = stdout.read(CHUNK)
                if not chunk:
                    break
                f.write(chunk)
                received += len(chunk)
                if expected:  # compressed: an estimate, never "done" before the end
                    rep.progress(min(0.95, received / expected), f"{received / 2**20:.1f} MiB")
        code = stdout.channel.recv_exit_status()
        reply = _last_json(stderr.read().decode("utf-8", "replace")) or {}
        if code or not reply.get("ok"):
            raise SavesError(reply.get("error") or f"the Frame couldn't back up the saves (exit {code})")
        manifest = read_manifest(part)
        if manifest.get("package") != pkg:
            raise SavesError(f"the Frame sent saves of {manifest.get('package')}, not {pkg}")
        part.replace(dest)
    except BaseException:
        try:
            stdout.channel.close()
        except Exception:  # noqa: BLE001
            pass
        part.unlink(missing_ok=True)
        raise
    data = _sidecar_data(manifest, dest)
    sidecar_of(dest).write_text(json.dumps(data, indent=1), encoding="utf-8")
    result = _from_sidecar(dest, data)
    prune(pkg, keep, protect=dest)
    rep.progress(1.0, "")
    rep.log(f"saved {result.files} file(s), {result.bytes / 2**20:.1f} MiB of saves, to {dest}"
            + (f"; {result.skipped} large file(s) left out" if result.skipped else ""))
    return result


def restore(frame, pkg: str, archive: Path, reporter: Reporter | None = None) -> dict:
    """Send a backup to the Frame; its current saves are kept there as a rollback copy (`undo_restore`)."""
    rep = reporter or Reporter()
    archive = Path(archive)
    manifest = read_manifest(archive)  # a damaged or foreign file fails here, before the Frame is touched
    if manifest.get("package") != pkg:
        raise SavesError(f"this backup is of {manifest.get('package')}, not {pkg}")
    rep.stage("saves", "Restoring saves")
    size = archive.stat().st_size
    stdin, stdout, stderr = _exec(frame, "_restore_saves", {"package": pkg})
    sent = 0
    try:
        with open(archive, "rb") as f:
            while True:
                rep.check_cancel()
                chunk = f.read(CHUNK)
                if not chunk:
                    break
                try:
                    stdin.write(chunk)
                except OSError:
                    break  # the Frame stopped reading (refused): its reply says why
                sent += len(chunk)
                rep.progress(min(0.95, sent / max(1, size)), f"{sent / 2**20:.1f} MiB")
        try:
            stdin.channel.shutdown_write()
        except OSError:
            pass
    except BaseException:
        try:
            stdout.channel.close()  # a cancelled restore: the agent sees a cut-off archive and changes nothing
        except Exception:  # noqa: BLE001
            pass
        raise
    out = stdout.read().decode("utf-8", "replace")
    code = stdout.channel.recv_exit_status()
    reply = _last_json(out) or {}
    if not reply.get("ok"):
        err = stderr.read().decode("utf-8", "replace").strip()
        raise SavesError(reply.get("error") or f"the Frame couldn't restore the saves (exit {code}) {err[-300:]}")
    rep.progress(1.0, "")
    return reply["result"]


def undo_restore(frame, pkg: str) -> dict:
    """Put back the saves the last restore replaced (the Frame's rollback copy; the replaced ones become the new
    rollback copy)."""
    stdin, stdout, stderr = _exec(frame, "_restore_saves", {"package": pkg, "rollback": True})
    try:
        stdin.channel.shutdown_write()
    except OSError:
        pass
    reply = _last_json(stdout.read().decode("utf-8", "replace")) or {}
    stdout.channel.recv_exit_status()
    if not reply.get("ok"):
        raise SavesError(reply.get("error") or "the Frame couldn't put the saves back")
    return reply["result"]

