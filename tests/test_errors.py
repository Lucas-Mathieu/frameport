"""Plain explanations for common failures (frameport.errors)."""
import errno
import zipfile

import paramiko
import pytest
from paramiko.ssh_exception import NoValidConnectionsError

from frameport.errors import explain, is_connection_error
from frameport.frame.connection import AgentFailed

OFFLINE = [NoValidConnectionsError({("10.0.0.9", 22): OSError("refused")}), TimeoutError("timed out"),
           paramiko.SSHException("SSH session not active"), EOFError(),
           OSError(errno.EHOSTUNREACH, "No route to host")]


@pytest.mark.parametrize("exc", OFFLINE)
def test_frame_offline(exc):
    assert is_connection_error(exc)
    assert explain(exc).startswith("Can't reach your Frame")


@pytest.mark.parametrize("exc,start", [
    (paramiko.ChannelException(2, "Connect failed"), "The connection to the Frame is busy"),
    (RuntimeError("not enough space on the Frame: need 30.1 GiB + 1 GiB headroom, have 12.0 GiB"),
     "Not enough space on the Frame (need 30.1 GiB"),
    (AgentFailed("Batman is running on the Frame. Close it first."), "A game is running on the Frame"),
    (AgentFailed("Steam did not close; library not modified"), "Steam on the Frame isn't responding"),
    (zipfile.BadZipFile("File is not a zip file"), "The APK file is damaged"),
    (KeyError("There is no item named 'AndroidManifest.xml' in the archive"), "The APK file is damaged"),
    (OSError(errno.ENOSPC, "No space left on device"), "This PC's disk is full"),
    (AgentFailed("uploaded APK checksum mismatch (transfer corrupted?)"), "The upload to the Frame was damaged"),
])
def test_known_failures(exc, start):
    assert explain(exc).startswith(start) and not is_connection_error(exc)


def test_other_errors_keep_their_message():
    auth = ConnectionError("SSH authentication failed (bad key). Run the FramePort bootstrap on the Frame.")
    # login refused: points at the first-time setup, and isn't a "Frame went away" (the queue mustn't wait for it)
    assert explain(auth).startswith("This Frame isn't set up") and not is_connection_error(auth)
    assert explain(AgentFailed("Lepton is not installed (Developer Mode → …)")).startswith("Lepton is not installed")
    assert explain(RuntimeError()) == "RuntimeError"


@pytest.mark.parametrize("wrapped", [False, True])
def test_certificate_verification_explanation(wrapped):
    import ssl

    import requests

    exc = ssl.SSLCertVerificationError(1, "self-signed certificate in certificate chain")
    if wrapped:
        exc = requests.exceptions.SSLError(exc)
    message = explain(exc)
    assert "HTTPS certificate couldn't be verified" in message
    assert "another network" in message and "HTTPS scanning" in message
    assert not is_connection_error(exc)


def test_unrelated_ssl_error_keeps_message():
    import requests

    assert explain(requests.exceptions.SSLError("TLS protocol mismatch")) == "TLS protocol mismatch"


@pytest.mark.parametrize("stage", ["metadata", "archive"])
def test_java_certificate_failure_reaches_explain(monkeypatch, stage):
    from unittest.mock import Mock

    import requests

    from frameport.tools import toolchain

    exc = requests.exceptions.SSLError("[SSL: CERTIFICATE_VERIFY_FAILED] certificate verify failed")
    get = Mock(side_effect=exc)
    if stage == "archive":
        get = Mock(return_value=Mock(json=lambda: [{"binary": {"package": {
            "link": "https://example.invalid/java.tar.gz", "name": "java.tar.gz", "checksum": "unused"}},
            "version": {"semver": "test"}}]))
        monkeypatch.setattr(toolchain.cache, "download", Mock(side_effect=exc))
    monkeypatch.setattr(toolchain.cache, "http_get", get)
    with pytest.raises(requests.exceptions.SSLError) as caught:
        toolchain.install_java()
    assert "another network" in explain(caught.value)
    assert "verify" not in get.call_args.kwargs
