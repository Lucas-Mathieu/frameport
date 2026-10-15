"""System OpenSSH for bulk files; the established Frame connection owns trust and progress polling."""
from __future__ import annotations

import contextlib
import csv
import io
import os
import posixpath
import shutil
import signal
import subprocess
import tarfile
import tempfile
import threading
import time
from pathlib import Path, PureWindowsPath

from .connection import sh_quote, ssh_dir


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


@contextlib.contextmanager
def credentials(frame, port_flag="-P"):
    """Private temporary copies avoid modifying pairing keys, including their Windows ACLs."""
    key_paths = [ssh_dir() / name for name in ("id_ed25519", "id_rsa") if (ssh_dir() / name).exists()]
    if not key_paths:
        raise OSError("no FramePort key available for OpenSSH")
    key = frame.client.get_transport().get_remote_server_key()
    with tempfile.TemporaryDirectory(prefix="frameport-upload-") as directory:
        directory = Path(directory)
        sid = None
        if os.name == "nt":
            row = next(csv.reader(io.StringIO(subprocess.check_output(
                ["whoami", "/user", "/fo", "csv", "/nh"], text=True, **hidden()))))
            sid = row[-1]
            subprocess.run(["icacls", str(directory), "/inheritance:r", "/grant:r", f"*{sid}:(OI)(CI)F"],
                           check=True, stdout=subprocess.DEVNULL, **hidden())
        else:
            directory.chmod(0o700)
        copies = []
        for path in key_paths:
            copy = directory / path.name
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
        yield ["-F", os.devnull, *copies, "-o", "BatchMode=yes", "-o", "IdentitiesOnly=yes",
               "-o", "StrictHostKeyChecking=yes", "-o", f'UserKnownHostsFile="{known.as_posix()}"',
               "-o", f"GlobalKnownHostsFile={os.devnull}", "-o", "ConnectTimeout=10",
               "-o", "ServerAliveInterval=10", "-o", "ServerAliveCountMax=3",
               port_flag, str(frame.target.port)]


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
    with credentials(frame) as args, contextlib.ExitStack() as stack:
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
                        detail = errors.read().decode(errors="replace")[-1500:]
                        raise OSError(f"OpenSSH SFTP failed ({process.returncode}): {detail}")
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
    executable = shutil.which("ssh")
    if not executable:
        raise OSError("system OpenSSH SSH is unavailable")
    reporter.check_cancel()
    with credentials(frame, port_flag="-p") as args, tempfile.TemporaryFile() as errors:
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
            if failures:
                raise failures[0]
            if process.poll() != 0:
                errors.seek(0)
                raise OSError(f"OpenSSH tar failed ({process.returncode}): "
                              + errors.read().decode(errors="replace")[-1500:])
        finally:
            stop_process(process)
            writer.join()
            with contextlib.suppress(OSError):
                process.stdin.close()
