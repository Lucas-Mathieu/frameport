"""System OpenSSH for bulk files; the established Frame connection owns trust and progress polling."""
from __future__ import annotations

import contextlib
import os
import posixpath
import re
import shutil
import signal
import subprocess
import tarfile
import tempfile
import threading
import time
from pathlib import Path, PureWindowsPath

from .connection import extra_keys, sh_quote, ssh_dir


def quote(value, windows=False) -> str:
    """One sftp batch argument, taken literally.

    Inside double quotes sftp escapes glob characters (* ? [) itself and undoes that for put's destination, so
    they must NOT be escaped again: an extra backslash became part of the name (GitHub #324: "[ENG]" in a folder
    name made every OpenSSH upload fail, on Linux too). Only backslash and the quote are escaped. ``windows``: a
    local Windows path goes with forward slashes; Win32-OpenSSH's sftp turns every backslash of a local path into
    '/', escapes included ("C:\\\\Users" → "C://Users").
    """
    value = str(value)
    if any(c in value for c in "\r\n\x00"):
        raise ValueError("SFTP paths cannot contain newlines or NUL")
    if windows:
        value = PureWindowsPath(value).as_posix()
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def quote_local(path) -> str:
    return quote(path, windows=os.name == "nt")


def size_of(sftp, path):
    try:
        return sftp.stat(path).st_size
    except FileNotFoundError:
        return 0


def key_files(relaxed=False) -> list[Path]:
    """The private keys OpenSSH offers: FramePort's own (<data>/ssh) and the usable key files from Settings;
    ``relaxed`` also this PC's default keys (default_key_files)."""
    keys = [ssh_dir() / name for name in ("id_ed25519", "id_rsa") if (ssh_dir() / name).exists()]
    for path in [path for path, _ in extra_keys()] + (default_key_files() if relaxed else []):
        if path not in keys:
            keys.append(path)
    return keys


def default_key_files() -> list[Path]:
    """This PC's own keys that Paramiko's "agent" login tries (look_for_keys: ~/.ssh/id_rsa, id_dsa, id_ecdsa,
    id_ed25519; on Windows ~/ssh too), usable without a passphrase. OpenSSH needs them named: it skips its default
    keys as soon as one -i is given, and Windows OpenSSH refuses a key file others can read (a private copy can't
    be refused)."""
    from .connection import KeyFileError, load_key_file

    folders = [Path.home() / ".ssh"] + ([Path.home() / "ssh"] if os.name == "nt" else [])
    found = []
    for folder in folders:
        for name in ("id_rsa", "id_dsa", "id_ecdsa", "id_ed25519"):
            path = folder / name
            if not path.is_file():
                continue
            try:
                load_key_file(path)
            except KeyFileError:
                continue  # passphrase-protected or not a key: only the agent can use it
            found.append(path)
    return found


def current_sid() -> str:
    """This Windows account's SID. whoami prints in the console code page: read bytes, so a user name with
    accents can't break decoding (only the ASCII SID is used)."""
    out = subprocess.check_output(["whoami", "/user", "/fo", "csv", "/nh"], **hidden())
    match = re.search(rb"S-1-[0-9]+(?:-[0-9]+)+", out)
    if not match:
        raise OSError("cannot find this Windows account's SID for the key copies' permissions")
    return match.group().decode()


@contextlib.contextmanager
def credentials(frame, port_flag="-P", relaxed=False):
    """OpenSSH arguments that log in like the Frame connection and trust only the host key it pinned.

    Keys are offered as private temporary copies (that avoids modifying pairing keys, including their Windows
    ACLs; Windows OpenSSH refuses key files others can read). Strict (default): FramePort's keys and the key
    files from Settings only (IdentitiesOnly). ``relaxed``: also this PC's default ~/.ssh keys and SSH agent, the
    way Paramiko's "agent" login does (GitHub #242: a Frame that only authorizes the user's own key). Verbose
    (-v), so a refused login can say which keys were offered (failure()).
    """
    key_paths = key_files(relaxed)
    if not key_paths and not relaxed:
        raise OSError("no FramePort key available for OpenSSH")
    key = frame.client.get_transport().get_remote_server_key()
    with tempfile.TemporaryDirectory(prefix="frameport-upload-") as directory:
        directory = Path(directory)
        sid = None
        if os.name == "nt":
            sid = current_sid()
            subprocess.run(["icacls", str(directory), "/inheritance:r", "/grant:r", f"*{sid}:(OI)(CI)F"],
                           check=True, stdout=subprocess.DEVNULL, **hidden())
        else:
            directory.chmod(0o700)
        copies = []
        for n, path in enumerate(key_paths):
            copy = directory / f"key{n}"  # own names: a key from Settings may be called id_ed25519 too
            shutil.copyfile(path, copy)
            if sid:
                subprocess.run(["icacls", str(copy), "/inheritance:r", "/remove:g", "*S-1-3-4",
                                "/grant:r", f"*{sid}:F"], check=True, stdout=subprocess.DEVNULL, **hidden())
            else:
                copy.chmod(0o600)
            copies.extend(["-i", str(copy)])
        known = directory / "known_hosts"
        host = frame.target.host if frame.target.port == 22 else f"[{frame.target.host}]:{frame.target.port}"
        known.write_text(f"{host} {key.get_name()} {key.get_base64()}\n")
        yield ["-F", os.devnull, "-v", *copies, "-o", "BatchMode=yes",
               *([] if relaxed else ["-o", "IdentitiesOnly=yes"]),
               "-o", "StrictHostKeyChecking=yes", "-o", f'UserKnownHostsFile="{known.as_posix()}"',
               "-o", f"GlobalKnownHostsFile={os.devnull}", "-o", "ConnectTimeout=10",
               "-o", "ServerAliveInterval=10", "-o", "ServerAliveCountMax=3",
               port_flag, str(frame.target.port)]


class OpenSSHAuthFailed(OSError):
    """OpenSSH reached the Frame but the login was refused (the keys it offered aren't authorized there)."""


AUTH_REFUSED = ("Permission denied (", "Too many authentication failures", "No more authentication methods")
# debug lines (-v) worth keeping: which keys were found, offered, refused or unreadable
AUTH_DEBUG = ("identity file", "Offering public key", "Server accepts key", "Will attempt key",
              "Authentications that can continue", "Authenticated to", "Load key", "no such identity",
              "Skipping", "agent", "Next authentication method")


def failure(what: str, code, stderr: bytes) -> OSError:
    """The error for a failed OpenSSH run: OpenSSH's own messages (all non-debug lines: warnings such as
    "UNPROTECTED PRIVATE KEY FILE" included) plus the -v lines about keys; OpenSSHAuthFailed for a refused login."""
    text = stderr.decode(errors="replace").replace("\r\n", "\n")
    lines = [line for line in text.splitlines() if line.strip() and (
        not line.startswith("debug") or any(k in line for k in AUTH_DEBUG))]
    detail = "\n".join(lines)
    if len(detail) > 4000:
        detail = "…" + detail[-4000:]
    refused = code == 255 and any(marker in text for marker in AUTH_REFUSED)
    return (OpenSSHAuthFailed if refused else OSError)(f"{what} failed ({code}): {detail}")


def login_modes(frame) -> list[bool]:
    """How OpenSSH logs in, in order (False = FramePort's keys only, True = also this PC's keys and agent).

    A Frame connection that logged in with this PC's own keys/agent (Paramiko's "agent" attempt) goes straight to
    relaxed: FramePort's key isn't authorized there. Else strict first, relaxed once more after a refused login.
    A mode that worked is kept for the rest of the connection. FramePort doesn't add its key to the Frame for
    this: it only does that after a password login, where the user typed the Frame's password for it."""
    known = getattr(frame, "openssh_relaxed", None)
    if known is not None:
        return [bool(known)]
    if getattr(frame, "auth", "") == "agent":
        return [True]
    return [False, True]


def with_login_modes(frame, reporter, run):
    """run(relaxed) in each login mode until one isn't refused (see login_modes)."""
    modes = login_modes(frame)
    if modes == [True] and getattr(frame, "openssh_relaxed", None) is None:
        reporter.log("the Frame connection logged in with this PC's own SSH keys or agent: OpenSSH uses them too")
    for n, relaxed in enumerate(modes):
        try:
            result = run(relaxed)
        except OpenSSHAuthFailed as exc:
            if n == len(modes) - 1:
                raise
            reporter.log(f"OpenSSH login with FramePort's SSH keys was refused; trying this PC's own SSH keys and "
                         f"agent too. {exc}")
            continue
        with contextlib.suppress(AttributeError):
            frame.openssh_relaxed = relaxed
        return result


def hidden():
    return {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}


def stop_process(process):
    """SFTP starts an ssh child: stop the entire owned process group before resuming with another backend."""
    if process.poll() is None:
        if os.name == "nt":
            subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10, **hidden())
            if process.poll() is None:
                process.kill()
        else:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
    process.wait()


def upload(frame, items, remote_root, reporter, total, sent, workers):
    """One persistent SFTP batch per worker. A failure leaves .part files for Paramiko to resume.

    Only successful batches are promoted, after every process has exited successfully. All control operations
    happen in the supervising thread, so Paramiko's SFTP client is never shared across threads.
    """
    return with_login_modes(frame, reporter, lambda relaxed: _upload(
        frame, items, remote_root, reporter, total, sent, workers, relaxed))


def _upload(frame, items, remote_root, reporter, total, sent, workers, relaxed):
    executable = shutil.which("sftp")
    if not executable:
        raise OSError("system OpenSSH SFTP is unavailable")
    from ..install.installer import Speed

    reporter.check_cancel()
    if not items:
        return sent
    groups = [[] for _ in range(min(workers, len(items)))]
    sizes, offsets, progress = {}, {}, {}
    for path, rel, size in sorted(items, key=lambda item: item[2], reverse=True):
        remote = posixpath.join(remote_root, rel)
        quote_local(path)
        quote(remote)
        offset = size_of(frame.sftp, remote + ".part")
        if offset > size:
            frame.sftp.remove(remote + ".part")
            offset = 0
        offsets[rel] = progress[rel] = offset
        sizes[rel] = size
        if offset == size:
            continue  # complete from an earlier attempt (sftp's reput refuses a target of the same size): rename only
        group = min(groups, key=lambda group: sum(item[2] for item in group))
        group.append((path, rel, size))
    groups = [group for group in groups if group]
    speed = Speed(total - sent - sum(offsets.values()))
    reporter.log(f"uploading large files with OpenSSH SFTP ({len(groups)} workers)")
    setup_started = time.monotonic()
    with credentials(frame, relaxed=relaxed) as args, contextlib.ExitStack() as stack:
        monitor = stack.enter_context(frame.client.open_sftp())
        monitor.get_channel().settimeout(2)
        reporter.log(f"OpenSSH credentials and progress connection ready in {time.monotonic() - setup_started:.2f}s")
        processes = []
        try:
            for group in groups:
                reporter.check_cancel()
                batch = stack.enter_context(tempfile.TemporaryFile())
                for path, rel, _ in group:
                    remote = posixpath.join(remote_root, rel) + ".part"
                    command = "reput" if offsets[rel] else "put"
                    batch.write(f"{command} {quote_local(path)} {quote(remote)}\n".encode())
                batch.seek(0)
                errors = stack.enter_context(tempfile.TemporaryFile())
                process = subprocess.Popen([executable, *args, "-b", "-",
                                            f"{frame.target.user}@{frame.target.host}"], stdin=batch,
                                           stdout=subprocess.DEVNULL, stderr=errors,
                                           **(hidden() if os.name == "nt" else {"start_new_session": True}))
                processes.append((process, errors))
            while True:
                reporter.check_cancel()
                for process, errors in processes:
                    if process.poll() not in (None, 0):
                        errors.seek(0)
                        raise failure("OpenSSH SFTP", process.returncode, errors.read())
                for rel, size in sizes.items():
                    reporter.check_cancel()
                    progress[rel] = max(progress[rel], min(size, size_of(
                        monitor, posixpath.join(remote_root, rel) + ".part")))
                done = sum(progress.values())
                reporter.progress((sent + done) / max(total, 1),
                                  f"{len(items)} files · {done / 2**20:.0f}/{sum(sizes.values()) / 2**20:.0f} MiB",
                                  speed=speed.text(done - sum(offsets.values())))
                if all(process.poll() == 0 for process, _ in processes):
                    break
                time.sleep(0.2)
            for _, rel, size in items:
                reporter.check_cancel()
                remote = posixpath.join(remote_root, rel)
                if size_of(monitor, remote + ".part") != size:
                    raise OSError(f"OpenSSH upload has an unexpected size: {rel}")
            for start in range(0, len(items), 100):
                reporter.check_cancel()
                commands = []
                for _, rel, _ in items[start:start + 100]:
                    remote = posixpath.join(remote_root, rel)
                    commands.append(f"mv -f -- {sh_quote(remote + '.part')} {sh_quote(remote)}")
                code, _, error = frame.run(" && ".join(commands))
                if code:
                    raise OSError(f"cannot finalize OpenSSH upload: {error}")
        finally:
            for process, _ in processes:
                stop_process(process)
    return sent + sum(size for _, _, size in items)


def put_tar(frame, files, remote_root, reporter, progress):
    """Stream a tar batch over paired OpenSSH; supervise writes so Cancel also stops a blocked pipe."""
    return with_login_modes(frame, reporter,
                            lambda relaxed: _put_tar(frame, files, remote_root, reporter, progress, relaxed))


def _put_tar(frame, files, remote_root, reporter, progress, relaxed):
    executable = shutil.which("ssh")
    if not executable:
        raise OSError("system OpenSSH SSH is unavailable")
    reporter.check_cancel()
    with credentials(frame, port_flag="-p", relaxed=relaxed) as args, tempfile.TemporaryFile() as errors:
        command = f"mkdir -p {sh_quote(remote_root)} && tar -xf - -C {sh_quote(remote_root)}"
        process = subprocess.Popen([executable, *args, f"{frame.target.user}@{frame.target.host}", command],
                                   stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=errors,
                                   **(hidden() if os.name == "nt" else {"start_new_session": True}))
        failures = []

        def write():
            done = 0
            try:
                with tarfile.open(fileobj=process.stdin, mode="w|", format=tarfile.PAX_FORMAT) as archive:
                    for local, rel in files:
                        reporter.check_cancel()
                        archive.add(str(local), arcname=rel, recursive=False)
                        done += Path(local).stat().st_size
                        progress(done, rel)
                process.stdin.close()
            except Exception as exc:
                failures.append(exc)

        writer = threading.Thread(target=write, name="frameport-tar", daemon=True)
        writer.start()
        try:
            while writer.is_alive() or process.poll() is None:
                reporter.check_cancel()
                if failures or process.poll() not in (None, 0):
                    break
                writer.join(timeout=0.1)
                if not writer.is_alive() and process.poll() is None:
                    time.sleep(0.1)
            reporter.check_cancel()
            if failures and process.poll() is None:  # a refused login shows as a broken pipe first
                with contextlib.suppress(subprocess.TimeoutExpired):
                    process.wait(timeout=2)
            if process.poll() not in (None, 0):
                errors.seek(0)
                raise failure("OpenSSH tar", process.returncode, errors.read())
            if failures:
                raise failures[0]
        finally:
            stop_process(process)
            writer.join()
            with contextlib.suppress(OSError):
                process.stdin.close()
