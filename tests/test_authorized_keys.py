"""Adding FramePort's key to ~/.ssh/authorized_keys never glues it onto a last line without a newline (GitHub #242):
sshd ignores such a line, so OpenSSH uploads were refused while Paramiko got in with the user's own key."""
import re
import shutil
import subprocess

import pytest

from frameport.core.paths import bootstrap_dir
from frameport.frame.connection import install_key_command

KEY = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIFramePortTestKey frameport"
OTHER = "ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABAQUserKey user@pc"
pytestmark = pytest.mark.skipif(shutil.which("bash") is None, reason="needs bash")


def bootstrap_key_block() -> str:
    script = (bootstrap_dir() / "bootstrap.sh").read_text()
    start = script.index("mkdir -p ~/.ssh")
    return script[start:script.index("\n\n", start)]


def run(tmp_path, command, before, key=KEY):
    ssh = tmp_path / ".ssh"
    if before is not None:
        ssh.mkdir(mode=0o700)
        (ssh / "authorized_keys").write_text(before)
    subprocess.run(["bash", "-c", command], env={"HOME": str(tmp_path), "PATH": "/usr/bin:/bin", "key": key},
                   check=True)
    return (ssh / "authorized_keys").read_text()


def commands():
    return [("install_key", install_key_command(KEY)), ("bootstrap", bootstrap_key_block())]


@pytest.mark.parametrize("name,command", commands())
@pytest.mark.parametrize("before,after", [
    (None, KEY + "\n"),                                      # no file yet
    ("", KEY + "\n"),                                        # empty file
    (OTHER + "\n", OTHER + "\n" + KEY + "\n"),               # normal file
    (OTHER, OTHER + "\n" + KEY + "\n"),                      # last line without a newline: the #242 case
    (OTHER + "\n" + KEY + "\n", OTHER + "\n" + KEY + "\n"),  # already there: unchanged
])
def test_key_is_added_on_its_own_line(tmp_path, name, command, before, after):
    assert run(tmp_path, command, before) == after


@pytest.mark.parametrize("name,command", commands())
def test_glued_key_gets_a_working_line(tmp_path, name, command):
    # an earlier install glued the key onto another line: sshd can't use it, so a clean line is added
    text = run(tmp_path, command, OTHER + KEY + "\n")
    assert KEY in text.splitlines()


def test_file_mode(tmp_path):
    run(tmp_path, install_key_command(KEY), OTHER)
    assert oct((tmp_path / ".ssh" / "authorized_keys").stat().st_mode & 0o777) == "0o600"
    assert re.fullmatch(r"0o7\d\d", oct((tmp_path / ".ssh").stat().st_mode & 0o777))
