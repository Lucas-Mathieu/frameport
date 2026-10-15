"""Compare bulk transports on one paired Frame endpoint and one destination filesystem.

    uv run python scripts/upload_bench.py --mib 512 --repeat 3 --json /tmp/uploads.json
    uv run python scripts/upload_bench.py --fast-link --remote-dir /path/on/sdcard --check-resume

The script uses a unique scratch folder, verifies SHA-256 outside the timed
transfer, and removes its own files afterwards. Native SCP/SFTP timings include startup; SSH streaming reports
handshake separately. Windows-native Python and WSL must be measured separately.
Use --methods app-files-1,app-files-2,sftp-files-1,sftp-files-2 to compare parallel file uploads.
Both file methods accept worker counts 1, 2, 3, 4 and 8, using the same eight-file payload.
app-files-N uses the production backend (OpenSSH preferred, Paramiko fallback); its log records the actual choice.
Use --methods app-small,paramiko-small to compare production tar batches with the same small-file payload.
"""
from __future__ import annotations

import argparse
import contextlib
import csv
import hashlib
import io
import json
import os
import platform
import random
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

import paramiko  # noqa: E402

from frameport.frame.connection import Frame, saved_targets, sh_quote, ssh_dir  # noqa: E402


@contextlib.contextmanager
def private_key():
    """Temporary private copy; never alter the user's pairing key/ACL."""
    key = ssh_dir() / "id_ed25519"
    if os.name != "nt" and stat.S_IMODE(key.stat().st_mode) & 0o077 == 0:
        yield str(key)
        return
    with tempfile.TemporaryDirectory(prefix="frameport-bench-key-") as directory:
        copy = Path(directory) / "id_ed25519"
        if os.name == "nt":
            row = next(csv.reader(io.StringIO(subprocess.check_output(
                ["whoami", "/user", "/fo", "csv", "/nh"], text=True))))
            # Restrict the directory before writing the key, including under inherited Windows ACLs.
            subprocess.run(["icacls", directory, "/inheritance:r", "/grant:r", f"*{row[-1]}:(OI)(CI)F"],
                           check=True, stdout=subprocess.DEVNULL)
        shutil.copyfile(key, copy)
        if os.name == "nt":
            # Python 3.13's chmod can add an OWNER RIGHTS ACE that Windows OpenSSH refuses.
            subprocess.run(["icacls", str(copy), "/inheritance:r", "/remove:g", "*S-1-3-4",
                            "/grant:r", f"*{row[-1]}:F"], check=True, stdout=subprocess.DEVNULL)
        else:
            copy.chmod(0o600)
        yield str(copy)


def ssh_base(frame, key, port_flag="-p"):
    return ["-i", key, "-o", "BatchMode=yes", "-o", "IdentitiesOnly=yes", "-o", "ConnectTimeout=10",
            "-o", f"UserKnownHostsFile={ssh_dir() / 'known_hosts'}", "-o", "StrictHostKeyChecking=yes",
            port_flag, str(frame.target.port)]


def cpu():
    t = os.times()
    return t.user + t.system + t.children_user + t.children_system


def digest(path):
    with open(path, "rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def sftp_quote(path):
    """OpenSSH batch syntax, distinct from shell quoting."""
    value = str(path)
    if any(c in value for c in "\n\r\x00"):
        raise ValueError("SFTP paths cannot contain newlines or NUL")
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def native(frame, key, tool="ssh"):
    return [tool, *ssh_base(frame, key, "-p" if tool == "ssh" else "-P"),
            f"{frame.target.user}@{frame.target.host}"]


def stream_ssh(frame, key, command, writer, cipher=None):
    args = native(frame, key)
    if cipher:
        args[1:1] = ["-c", cipher]
    t0 = time.monotonic()
    # Drain stderr to a file: no pipe deadlock for long-running transfers or an authentication failure.
    with tempfile.TemporaryFile() as errors:
        process = subprocess.Popen([*args, f"printf 'READY\\n'; {command}"], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=errors)
        try:
            if process.stdout.readline() != b"READY\n":
                process.wait(timeout=15)
                errors.seek(0)
                raise RuntimeError(errors.read().decode(errors="replace")[-1000:])
            setup = time.monotonic() - t0
            writer(process.stdin)
            process.stdin.close()
            if process.wait(timeout=60):
                errors.seek(0)
                raise RuntimeError(errors.read().decode(errors="replace")[-1000:])
            return setup
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            process.stdout.close()
            if not process.stdin.closed:
                process.stdin.close()


def send(method, frame, local, remote, key, small, large=None, logs=None):
    """Returns measured setup seconds (None when the native tool cannot expose handshake completion)."""
    t0 = time.monotonic()
    if method in ("app-small", "paramiko-small") or method.startswith(("app-files-", "paramiko-files-")):
        from frameport.core.events import Reporter
        from frameport.install.installer import upload_files

        workers = 1 if method.endswith("-small") else int(method.rsplit("-", 1)[1])
        with contextlib.closing(Frame(frame.target, frame.password).connect()) as transfer:
            items = [(path, path.name, path.stat().st_size) for path in (small if method.endswith("-small") else large)]
            def record(event):
                if event.kind == "log":
                    if logs is not None:
                        logs.append(event.message)
                    print(f"  {event.message}", flush=True)
            reporter = Reporter([record])
            upload_files(transfer, items, remote, reporter, sum(size for _, _, size in items), workers=workers,
                         prefer_native=not method.startswith("paramiko-"))
        return None  # worker handshakes overlap transfers; measure the production end-to-end operation
    if method.startswith("sftp-files-"):
        workers = min(int(method.rsplit("-", 1)[1]), len(large))
        frame.run(f"mkdir -p {sh_quote(remote)}")
        groups = [[] for _ in range(workers)]
        for path in sorted(large, key=lambda p: p.stat().st_size, reverse=True):
            group = min(groups, key=lambda group: sum(p.stat().st_size for p in group))
            group.append(path)
        def upload(group):
            args = native(frame, key, "sftp")
            args[-1:-1] = ["-b", "-"]
            commands = "".join(f"put {sftp_quote(path)} {sftp_quote(remote + '/' + path.name)}\n"
                               for path in group)
            subprocess.run(args, input=commands.encode(), check=True, stdout=subprocess.DEVNULL, timeout=1800)
        with ThreadPoolExecutor(max_workers=workers) as pool:
            list(pool.map(upload, groups))
        return None  # each worker keeps one native SFTP session for its entire batch
    if method in ("app", "app-tar"):
        with contextlib.closing(Frame(frame.target).connect()) as transfer:
            setup = time.monotonic() - t0
            if method == "app":
                transfer.put(local, remote)
            else:
                transfer.put_tar([(p, p.name) for p in small], remote)
        return setup
    if method == "paramiko-tuned":
        # Authenticate the same paired host key, rather than opening an unchecked raw Transport.
        with contextlib.closing(Frame(frame.target).connect()) as transfer:
            transport = transfer.client.get_transport()
            with paramiko.SFTPClient.from_transport(transport, window_size=64 << 20) as client:
                setup = time.monotonic() - t0
                with open(local, "rb") as source, client.open(remote, "wb") as target:
                    target.set_pipelined(True)
                    shutil.copyfileobj(source, target, 1 << 20)
        return setup
    if method.startswith("ssh"):
        cipher = {"ssh-gcm": "aes128-gcm@openssh.com", "ssh-chacha": "chacha20-poly1305@openssh.com"}.get(method)
        if method == "ssh-tar":
            def writer(pipe):
                with tarfile.open(fileobj=pipe, mode="w|", format=tarfile.PAX_FORMAT) as archive:
                    for path in small:
                        archive.add(path, arcname=path.name, recursive=False)
            command = f"mkdir -p {sh_quote(remote)} && tar -xf - -C {sh_quote(remote)}"
        else:
            def writer(pipe):
                with open(local, "rb") as source:
                    shutil.copyfileobj(source, pipe, 1 << 20)
            command = f"cat > {sh_quote(remote)}"
        return stream_ssh(frame, key, command, writer, cipher)
    if method == "sftp":
        args = native(frame, key, "sftp")
        args[-1:-1] = ["-b", "-"]
        subprocess.run(args, input=f"put {sftp_quote(local)} {sftp_quote(remote)}\n".encode(),
                       check=True, stdout=subprocess.DEVNULL, timeout=1800)
    elif method == "scp":
        args = native(frame, key, "scp")
        destination = args.pop()
        subprocess.run([*args, str(local), f"{destination}:{remote}"], check=True, timeout=1800)
    else:
        raise ValueError(f"unknown method {method}")
    return None


METHODS = ("app", "paramiko-tuned", "ssh", "ssh-gcm", "ssh-chacha", "sftp", "scp", "app-tar", "ssh-tar",
           "app-files-1", "app-files-2", "app-files-3", "app-files-4", "app-files-8",
           "paramiko-files-1", "paramiko-files-2", "paramiko-files-3", "paramiko-files-4", "paramiko-files-8",
           "sftp-files-1", "sftp-files-2", "sftp-files-3", "sftp-files-4", "sftp-files-8",
           "app-small", "paramiko-small")


def check_resume(frame, local, remote, key):
    """Interrupt the app mid-upload, then resume exactly that .part with app and native SFTP reput."""
    class Interrupted(Exception):
        pass

    results = []
    for method in ("app", "sftp"):
        if method == "sftp" and not shutil.which("sftp"):
            results.append({"method": method, "skipped": "sftp unavailable"})
            continue
        path = remote + "-" + method
        def interrupt(done, size):
            if done >= size // 2:
                raise Interrupted()
        try:
            frame.put(local, path, interrupt)
        except Interrupted:
            pass
        offset = frame.sftp.stat(path + ".part").st_size
        if not 0 < offset < local.stat().st_size:
            raise RuntimeError("interrupted upload did not leave a partial file")
        started = time.monotonic()
        if method == "app":
            frame.put(local, path)
        else:
            args = native(frame, key, "sftp")
            args[-1:-1] = ["-b", "-"]
            part = path + ".part"
            commands = f"reput {sftp_quote(local)} {sftp_quote(part)}\nrename {sftp_quote(part)} {sftp_quote(path)}\n"
            subprocess.run(args, input=commands.encode(), check=True, timeout=1800, stdout=subprocess.DEVNULL)
        code, out, err = frame.run(f"sha256sum {sh_quote(path)}")
        if code or out.split()[0] != digest(local):
            raise RuntimeError(f"resume checksum mismatch: {err}")
        results.append({"method": method, "offset": offset, "seconds": time.monotonic() - started,
                        "sha256_ok": True, "cancelled_app": True})
    from frameport.core.events import Cancelled, Reporter
    from frameport.install.installer import upload_files

    root = remote + "-production"
    items = [(local, "resume.obb", local.stat().st_size)]
    report = Reporter()
    report.subscribe(lambda event: report.cancelled.set()
                     if event.kind == "progress" and event.fraction >= 0.5 else None)
    try:
        upload_files(frame, items, root, report, local.stat().st_size, workers=1)
    except Cancelled:
        pass
    offset = frame.sftp.stat(root + "/resume.obb.part").st_size
    if not 0 < offset < local.stat().st_size:
        raise RuntimeError("production cancellation did not leave a partial file")
    started = time.monotonic()
    logs = []
    upload_files(frame, items, root, Reporter([lambda event: logs.append(event.message)
                                             if event.kind == "log" else None]), local.stat().st_size, workers=1)
    code, out, err = frame.run(f"sha256sum {sh_quote(root + '/resume.obb')}")
    if code or out.split()[0] != digest(local):
        raise RuntimeError(f"production resume checksum mismatch: {err}")
    results.append({"method": "production", "offset": offset, "seconds": time.monotonic() - started,
                    "sha256_ok": True, "cancelled_production": True, "logs": logs})
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--mib", type=int, default=512)
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--small-files", type=int, default=1024)
    parser.add_argument("--host")
    parser.add_argument("--fast-link", action="store_true")
    parser.add_argument("--remote-dir", help="destination filesystem, default: paired user's home")
    parser.add_argument("--methods", default=",".join(METHODS[:9]))
    parser.add_argument("--check-resume", action="store_true")
    parser.add_argument("--json", type=Path)
    parser.add_argument("--probe", action="store_true", help="show reachable links and storage without transfers")
    args = parser.parse_args()
    methods = args.methods.split(",")
    if args.mib < 1 or args.repeat < 1 or not 1 <= args.small_files <= 10000 or set(methods) - set(METHODS):
        parser.error("positive size/repeat and 1–10000 small files required; methods: " + ",".join(METHODS))
    targets = saved_targets()
    if not targets:
        parser.error("pair a Frame first")
    target = targets[0]
    if args.host:
        target.host = args.host
    report = {"platform": platform.platform(), "python": sys.version, "paramiko": paramiko.__version__,
              "wsl": "microsoft" in platform.release().lower(), "methods": [], "resume": [],
              "cpu_scope": "Python parent only on Windows; parent + child processes on POSIX"}
    with contextlib.ExitStack() as stack:
        control = stack.enter_context(contextlib.closing(Frame(target).connect()))
        frame, label = control.fast_link() if args.fast_link else (control, "explicit/current endpoint")
        if frame is not control:
            stack.callback(frame.close)
        report.update(host=frame.target.host, port=frame.target.port, link=label or "home/current endpoint",
                      addresses=control.addresses())
        _, inventory, _ = frame.run("lsblk -J -o NAME,TYPE,FSTYPE,MOUNTPOINTS")
        _, wireless, _ = frame.run("iw dev wlan0 link 2>/dev/null")
        report.update(storage_inventory=inventory, wireless=wireless)
        if args.probe:
            print(json.dumps(report, indent=2))
            return 0
        # One unique directory on the chosen filesystem; safely quote user-supplied locations.
        parent = args.remote_dir or frame.home
        code, scratch, err = frame.run(f"mktemp -d -p {sh_quote(parent)} frameport-bench.XXXXXXXX")
        if code:
            raise RuntimeError(err)
        scratch = scratch.strip()
        if not scratch or not scratch.startswith(parent.rstrip("/") + "/frameport-bench."):
            raise RuntimeError("unexpected scratch directory")
        stack.callback(frame.run, f"rm -rf -- {sh_quote(scratch)}")
        _, storage, _ = frame.run(f"df -T {sh_quote(scratch)}")
        report.update(destination=parent, storage=storage.strip())
        key = stack.enter_context(private_key())
        folder = Path(stack.enter_context(tempfile.TemporaryDirectory(prefix="frameport-upload-bench-")))
        local = folder / "payload.bin"
        # No compression-friendly repeated block: each method sends exactly the same random payload.
        with local.open("wb") as output:
            for _ in range(args.mib):
                output.write(os.urandom(1 << 20))
        expected = digest(local)
        small_dir = folder / "small"
        small_dir.mkdir()
        small = []
        with local.open("rb") as source:
            for i in range(args.small_files):
                path = small_dir / f"item-{i:05}.bin"
                source.seek((i * 65536) % local.stat().st_size)
                path.write_bytes(source.read(65536))
                small.append(path)
        small_hashes = {p.name: digest(p) for p in small}
        large_dir = folder / "large"
        large_dir.mkdir()
        large = []
        # Equal total bytes to the single-file trial; all worker counts send exactly these eight files.
        with local.open("rb") as source:
            for i in range(8):
                path = large_dir / f"main-{i:02}.obb"
                count = local.stat().st_size // 8 if i < 7 else local.stat().st_size - source.tell()
                with path.open("wb") as output:
                    remaining = count
                    while remaining:
                        block = source.read(min(remaining, 1 << 20))
                        output.write(block)
                        remaining -= len(block)
                large.append(path)
        large_hashes = {p.name: digest(p) for p in large}
        print(f"{platform.system()} {'WSL' if report['wsl'] else 'native'} "
              f"to {frame.target.host}, {parent}", flush=True)
        rng = random.Random(0)
        for repeat in range(args.repeat):
            order = list(methods)
            rng.shuffle(order)
            for method in order:
                row = {"method": method, "repeat": repeat + 1}
                tool = ("ssh" if method.startswith("ssh") else "sftp" if method.startswith("sftp-files-")
                        else method if method in ("scp", "sftp") else None)
                if tool and not shutil.which(tool):
                    row["skipped"] = f"no {tool} on PATH"
                    report["methods"].append(row)
                    continue
                remote = f"{scratch}/{method}-{repeat}"
                size = (sum(p.stat().st_size for p in small) if method.endswith(("-tar", "-small"))
                        else local.stat().st_size)
                c0, t0 = cpu(), time.monotonic()
                try:
                    row["logs"] = []
                    setup = send(method, frame, local, remote, key, small, large, row["logs"])
                    elapsed, used = time.monotonic() - t0, cpu() - c0
                    row.update(bytes=size, total_s=elapsed, setup_s=setup, cpu_s=used,
                               total_MB_s=size / elapsed / 1e6,
                               payload_MB_s=size / (elapsed - setup) / 1e6 if setup is not None else None)
                    verify_start = time.monotonic()
                    large_trial = method.startswith(("app-files-", "paramiko-files-", "sftp-files-"))
                    if method.endswith(("-tar", "-small")) or large_trial:
                        pattern = "*.obb" if large_trial else "*.bin"
                        _, out, _ = frame.run(f"cd {sh_quote(remote)} && sha256sum {pattern}")
                        actual = {line.split(maxsplit=1)[1].strip(): line.split()[0] for line in out.splitlines()}
                        verified = actual == (large_hashes if large_trial else small_hashes)
                    else:
                        code, out, _ = frame.run(f"sha256sum {sh_quote(remote)}")
                        verified = code == 0 and bool(out.split()) and out.split()[0] == expected
                    row.update(verify_s=time.monotonic() - verify_start, sha256_ok=verified)
                    if not verified:
                        raise RuntimeError("checksum mismatch")
                    print(f"{repeat + 1} {method:15} {size / elapsed / 1e6:7.1f} MB/s total "
                          f"{elapsed:6.2f}s setup={setup if setup is not None else 'included'} "
                          f"CPU={used:.2f}s checksum=OK",
                          flush=True)
                except Exception as exc:
                    row["error"] = str(exc)
                    print(f"{method}: {exc}", flush=True)
                report["methods"].append(row)
                # Exclude deletion and hash verification from transfer timings and retain no previous trial files.
                frame.run(f"rm -rf -- {sh_quote(remote)} {sh_quote(remote + '.part')}")
        if args.check_resume:
            try:
                report["resume"] = check_resume(frame, local, f"{scratch}/resume", key)
            except Exception as exc:
                report["resume"] = [{"error": str(exc)}]
                print(f"resume check: {exc}", flush=True)
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + "\n")
    return int(any(r.get("error") for r in [*report["methods"], *report["resume"]]))


if __name__ == "__main__":
    sys.exit(main())
