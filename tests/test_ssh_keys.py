"""SSH keys beyond FramePort's own (GitHub #242): key files from Settings, the login a connection used, and OpenSSH
uploads that fall back to this PC's own keys/agent when the Frame refuses FramePort's key."""
import contextlib
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock

import paramiko
import pytest
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ed25519

from frameport.core import library
from frameport.core.events import Reporter
from frameport.frame import connection as C
from frameport.frame import native_upload as native

REFUSED = (b"debug1: Reading configuration data nul\r\n"
           b"debug1: identity file C:/Temp/frameport-upload-x/key0 type 3\r\n"
           b"debug1: Connecting to 10.0.0.2 [10.0.0.2] port 22.\r\n"
           b"@         WARNING: UNPROTECTED PRIVATE KEY FILE!          @\r\n"
           b"debug1: Offering public key: C:/Temp/frameport-upload-x/key0 ED25519 SHA256:abc explicit\r\n"
           b"debug1: Authentications that can continue: publickey,password\r\n"
           b"debug1: kex: algorithm: curve25519-sha256\r\n"
           b"steamos@10.0.0.2: Permission denied (publickey,password).\r\n")


@pytest.fixture(autouse=True)
def no_real_home_keys(tmp_path, monkeypatch):
    """Never read the real ~/.ssh (relaxed mode looks there)."""
    empty = tmp_path / "empty-home"
    empty.mkdir()
    monkeypatch.setattr(native.Path, "home", lambda: empty)


def write_key(path: Path, password: bytes | None = None) -> Path:
    enc = serialization.BestAvailableEncryption(password) if password else serialization.NoEncryption()
    key = ed25519.Ed25519PrivateKey.generate()
    path.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.OpenSSH, enc))
    path.with_name(path.name + ".pub").write_bytes(key.public_key().public_bytes(
        serialization.Encoding.OpenSSH, serialization.PublicFormat.OpenSSH))
    return path


# ------------------------------------------------------------------------------------------- key files
def test_key_file_validation(tmp_path):
    good = write_key(tmp_path / "id_ed25519")
    assert isinstance(C.load_key_file(good), paramiko.PKey)
    with pytest.raises(C.KeyFileError, match="passphrase"):
        C.load_key_file(write_key(tmp_path / "locked", b"secret"))
    with pytest.raises(C.KeyFileError, match="public half"):
        C.load_key_file(tmp_path / "id_ed25519.pub")
    with pytest.raises(C.KeyFileError, match="not found"):
        C.load_key_file(tmp_path / "missing")
    (tmp_path / "notes.txt").write_text("hello")
    with pytest.raises(C.KeyFileError, match="isn't a private SSH key"):
        C.load_key_file(tmp_path / "notes.txt")


def test_extra_keys_come_from_settings_and_skip_unusable_ones(tmp_path):
    good = write_key(tmp_path / "mine")
    locked = write_key(tmp_path / "locked", b"secret")
    library.set_setting(C.EXTRA_KEYS_SETTING, [str(good), str(good), str(locked), str(tmp_path / "gone"), "", 3])
    assert C.extra_key_paths() == [good, locked, tmp_path / "gone"]
    assert [path for path, _ in C.extra_keys()] == [good]
    library.set_setting(C.EXTRA_KEYS_SETTING, str(good))  # a single path, e.g. edited by hand
    assert C.extra_key_paths() == [good]


class _Client:
    """paramiko.SSHClient stand-in: accepts the key `accepted` (or the agent when accepted == "agent")."""
    accepted = None
    tried: list = []

    def load_host_keys(self, path):
        pass

    def set_missing_host_key_policy(self, policy):
        pass

    def close(self):
        pass

    def save_host_keys(self, path):
        pass

    def get_transport(self):
        return Mock()

    def connect(self, pkey=None, password=None, allow_agent=False, look_for_keys=False, **kw):
        kind = "password" if password else "agent" if allow_agent else pkey
        _Client.tried.append(kind)
        if kind != _Client.accepted:
            raise paramiko.AuthenticationException("Authentication failed.")


@pytest.mark.parametrize("accepted,auth", [("app", "app_key"), ("user", "user_key"), ("agent", "agent")])
def test_connection_records_the_login_that_worked(monkeypatch, accepted, auth):
    monkeypatch.setattr(C.paramiko, "SSHClient", _Client)
    monkeypatch.setattr(C, "app_keys", lambda: ["app"])
    monkeypatch.setattr(C, "extra_keys", lambda: [(Path("/k"), "user")])
    monkeypatch.setattr(C.Frame, "run", lambda self, cmd: (0, "/home/steamos", ""))
    _Client.accepted, _Client.tried = accepted, []
    frame = C.Frame(C.FrameTarget("10.0.0.2")).connect()
    assert frame.auth == auth
    assert _Client.tried == ["app", "user", "agent"][:["app", "user", "agent"].index(accepted) + 1]


# ------------------------------------------------------------------------------------------- OpenSSH arguments
def frame_with_key():
    key = SimpleNamespace(get_name=lambda: "ssh-ed25519", get_base64=lambda: "PAIRED_KEY")
    client = SimpleNamespace(get_transport=lambda: SimpleNamespace(get_remote_server_key=lambda: key))
    return C.Frame(C.FrameTarget("paired.example"), client=client)


def test_credentials_offer_own_and_settings_keys_strict_or_relaxed(tmp_path, monkeypatch):
    own = tmp_path / "ssh"
    own.mkdir()
    write_key(own / "id_ed25519")
    mine = write_key(tmp_path / "id_ed25519")  # same file name as FramePort's: copies get their own names
    monkeypatch.setattr(native, "ssh_dir", lambda: own)
    library.set_setting(C.EXTRA_KEYS_SETTING, [str(mine)])
    with native.credentials(frame_with_key()) as args:
        copies = [Path(args[i + 1]) for i, a in enumerate(args) if a == "-i"]
        assert [c.read_bytes() for c in copies] == [(own / "id_ed25519").read_bytes(), mine.read_bytes()]
        assert "IdentitiesOnly=yes" in args and "BatchMode=yes" in args and "-v" in args
        assert all(oct(c.stat().st_mode & 0o777) == "0o600" for c in copies)
    assert not any(c.exists() for c in copies)
    with native.credentials(frame_with_key(), relaxed=True) as args:
        assert "IdentitiesOnly=yes" not in args and args.count("-i") == 2
        assert "StrictHostKeyChecking=yes" in args  # host key still pinned


def test_relaxed_credentials_work_without_any_key_file(tmp_path, monkeypatch):
    monkeypatch.setattr(native, "ssh_dir", lambda: tmp_path)
    with pytest.raises(OSError, match="no FramePort key"):
        with native.credentials(frame_with_key()):
            pass
    with native.credentials(frame_with_key(), relaxed=True) as args:
        assert "-i" not in args


def test_windows_sid_survives_a_non_ascii_user_name(monkeypatch):
    out = '"desktop-1\\josé müller","S-1-5-21-1004336348-1177238915-682003330-1001"\r\n'.encode("cp850")
    monkeypatch.setattr(native.subprocess, "check_output", lambda *a, **k: out)
    assert native.current_sid() == "S-1-5-21-1004336348-1177238915-682003330-1001"
    monkeypatch.setattr(native.subprocess, "check_output", lambda *a, **k: b"nothing")
    with pytest.raises(OSError):
        native.current_sid()


# ------------------------------------------------------------------------------------------- failures and modes
def test_failure_keeps_openssh_messages_and_key_lines():
    exc = native.failure("OpenSSH SFTP", 255, REFUSED)
    assert isinstance(exc, native.OpenSSHAuthFailed)
    text = str(exc)
    assert "UNPROTECTED PRIVATE KEY FILE" in text and "Offering public key" in text
    assert "identity file" in text and "Permission denied (publickey,password)" in text
    assert "kex: algorithm" not in text and "Reading configuration" not in text
    other = native.failure("OpenSSH SFTP", 1, b"stat x: No such file or directory\n")
    assert type(other) is OSError and "No such file" in str(other)
    assert type(native.failure("OpenSSH tar", 255, b"ssh: connect to host x port 22: Connection timed out")) is OSError


@pytest.mark.parametrize("auth,known,modes", [("app_key", None, [False, True]), ("user_key", None, [False, True]),
                                              ("password", None, [False, True]), ("agent", None, [True]),
                                              ("app_key", True, [True]), ("agent", False, [False])])
def test_login_modes(auth, known, modes):
    assert native.login_modes(SimpleNamespace(auth=auth, openssh_relaxed=known)) == modes
    assert native.login_modes(SimpleNamespace()) == [False, True]  # stand-ins without the fields


def sftp_setup(tmp_path, monkeypatch, refuse_strict=True, refuse_relaxed=False, auth="app_key"):
    """Two .part-less files; strict/relaxed logins refused as asked; returns the frame and the Popen calls."""
    paths = [tmp_path / "main.obb", tmp_path / "patch.obb"]
    for path in paths:
        path.write_bytes(b"0123456789")
    items = [(path, path.name, 10) for path in paths]
    files = {}
    monitor = Mock()
    monitor.__enter__ = Mock(return_value=monitor)
    monitor.__exit__ = Mock(return_value=False)

    def stat(path):
        if path not in files:
            raise FileNotFoundError(path)
        return SimpleNamespace(st_size=files[path])
    monitor.stat.side_effect = stat
    frame = SimpleNamespace(sftp=monitor, client=SimpleNamespace(open_sftp=lambda: monitor), auth=auth,
                            openssh_relaxed=None, target=C.FrameTarget("paired"), run=Mock(return_value=(0, "", "")))
    monkeypatch.setattr(native.shutil, "which", lambda _: "sftp")
    mode = {}

    @contextlib.contextmanager
    def creds(frame, port_flag="-P", relaxed=False):
        mode["relaxed"] = relaxed
        yield ["relaxed" if relaxed else "strict"]
    monkeypatch.setattr(native, "credentials", creds)
    calls = []

    def popen(args, **kwargs):
        relaxed = mode["relaxed"]
        calls.append(relaxed)
        refused = refuse_relaxed if relaxed else refuse_strict
        if refused:
            kwargs["stderr"].write(REFUSED)
            code = 255
        else:
            for _, rel, size in items:
                files["/incoming/" + rel + ".part"] = size
            code = 0
        process = Mock(returncode=code)
        process.poll.return_value = code
        return process
    monkeypatch.setattr(native.subprocess, "Popen", popen)
    return frame, items, calls


def test_refused_strict_login_retries_with_this_pcs_keys(tmp_path, monkeypatch):
    frame, items, calls = sftp_setup(tmp_path, monkeypatch)
    logs = []
    reporter = Reporter()
    reporter.log = lambda message, **k: logs.append(message)
    assert native.upload(frame, items, "/incoming", reporter, 20, 0, 2) == 20
    assert calls == [False, False, True, True]  # both strict workers are refused, then both run relaxed
    assert frame.openssh_relaxed is True
    assert any("trying this PC's own SSH keys" in m and "UNPROTECTED" in m for m in logs)
    assert native.login_modes(frame) == [True]  # the next upload starts in the mode that worked


def test_agent_login_goes_straight_to_relaxed(tmp_path, monkeypatch):
    frame, items, calls = sftp_setup(tmp_path, monkeypatch, auth="agent")
    assert native.upload(frame, items, "/incoming", Reporter(), 20, 0, 1) == 20
    assert calls == [True]


def test_refused_in_both_modes_raises_the_auth_failure(tmp_path, monkeypatch):
    frame, items, calls = sftp_setup(tmp_path, monkeypatch, refuse_relaxed=True)
    with pytest.raises(native.OpenSSHAuthFailed, match="Permission denied"):
        native.upload(frame, items, "/incoming", Reporter(), 20, 0, 1)
    assert calls == [False, True] and frame.openssh_relaxed is None
    assert not frame.run.called  # nothing promoted


def test_tar_retries_a_refused_login(tmp_path, monkeypatch):
    path = tmp_path / "small.bin"
    path.write_bytes(b"abc")
    frame = SimpleNamespace(target=C.FrameTarget("paired"), auth="app_key", openssh_relaxed=None)
    monkeypatch.setattr(native.shutil, "which", lambda _: "ssh")
    mode = {}

    @contextlib.contextmanager
    def creds(frame, port_flag="-P", relaxed=False):
        mode["relaxed"] = relaxed
        yield []
    monkeypatch.setattr(native, "credentials", creds)

    class Pipe:
        def write(self, data):
            if not mode["relaxed"]:
                raise BrokenPipeError(32, "Broken pipe")  # ssh already exited
            return len(data)

        def flush(self):
            pass

        def close(self):
            pass
    calls = []

    def popen(args, **kwargs):
        calls.append(mode["relaxed"])
        code = 0 if mode["relaxed"] else 255
        if code:
            kwargs["stderr"].write(REFUSED)
        process = Mock(stdin=Pipe(), returncode=code)
        process.poll.return_value = code
        return process
    monkeypatch.setattr(native.subprocess, "Popen", popen)
    native.put_tar(frame, [(path, "small.bin")], "/incoming", Reporter(), Mock())
    assert calls == [False, True] and frame.openssh_relaxed is True


def test_azure_ad_sid(monkeypatch):
    out = b'"azuread\\jmueller","S-1-12-1-1234567890-1234567890-1234567890-1234567890"\r\n'
    monkeypatch.setattr(native.subprocess, "check_output", lambda *a, **k: out)
    assert native.current_sid() == "S-1-12-1-1234567890-1234567890-1234567890-1234567890"


def test_relaxed_mode_names_this_pcs_default_keys(tmp_path, monkeypatch):
    """OpenSSH skips its default keys once any -i is given, so relaxed mode names the ones Paramiko tries."""
    home = tmp_path / "home"
    (home / ".ssh").mkdir(parents=True)
    rsa = home / ".ssh" / "id_rsa"
    from cryptography.hazmat.primitives.asymmetric import rsa as rsa_keys

    rsa.write_bytes(rsa_keys.generate_private_key(65537, 2048).private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.OpenSSH, serialization.NoEncryption()))
    write_key(home / ".ssh" / "id_ecdsa", b"secret")  # passphrase: only usable through the agent
    (home / ".ssh" / "id_ed25519").write_text("garbage")
    monkeypatch.setattr(native.Path, "home", lambda: home)
    own = tmp_path / "own"
    own.mkdir()
    write_key(own / "id_ed25519")
    monkeypatch.setattr(native, "ssh_dir", lambda: own)
    assert native.default_key_files() == [rsa]
    assert native.key_files() == [own / "id_ed25519"]
    assert native.key_files(relaxed=True) == [own / "id_ed25519", rsa]
    library.set_setting(C.EXTRA_KEYS_SETTING, [str(rsa)])  # a default key also named in Settings: offered once
    assert native.key_files(relaxed=True) == [own / "id_ed25519", rsa]


def test_add_remove_and_status_of_settings_keys(tmp_path):
    good = write_key(tmp_path / "frame key")
    with pytest.raises(C.KeyFileError, match="public half"):
        C.add_extra_key(tmp_path / "frame key.pub")
    with pytest.raises(C.KeyFileError, match="passphrase"):
        C.add_extra_key(write_key(tmp_path / "locked", b"pw"))
    assert C.add_extra_key(good) == good.resolve()
    C.add_extra_key(good)  # once only
    assert C.extra_key_paths() == [good.resolve()]
    good.unlink()
    [(path, problem)] = C.extra_key_status()
    assert path == good.resolve() and "not found" in problem
    assert C.remove_extra_key(good) is True and C.remove_extra_key(good) is False
    assert C.extra_key_paths() == []


def test_cli_ssh_keys(tmp_path):
    from typer.testing import CliRunner

    from frameport.cli import app

    good = write_key(tmp_path / "mine")
    run = CliRunner().invoke(app, ["frame", "ssh-keys", "--add", str(good)])
    assert run.exit_code == 0 and f"added {good.resolve()}" in run.output
    run = CliRunner().invoke(app, ["frame", "ssh-keys", "--add", str(tmp_path / "nope")])
    assert run.exit_code == 1 and str(good.resolve()) in run.output
    run = CliRunner().invoke(app, ["frame", "ssh-keys", "--remove", str(good.resolve())])
    assert run.exit_code == 0 and "no extra keys" in run.output
