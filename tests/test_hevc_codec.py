"""Codec packaging and per-game container isolation; no game assets needed."""
import hashlib
import importlib.util
import json
import os
import sys
import types
import zipfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]


def load_module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_assets_have_matching_checksums():
    directory = ROOT / "artifacts/hevc"
    manifest = json.loads((directory / "manifest.json").read_text())
    for name, expected in manifest["files"].items():
        path = directory / (name + ".txt" if name == "podman.py" else name)
        assert hashlib.sha256(path.read_bytes()).hexdigest() == expected
    assert (directory / "podman.py.txt").read_bytes() == (ROOT / "native/hevc/podman.py").read_bytes()


def test_mounts_only_matching_game_and_runtime(tmp_path, monkeypatch):
    wrapper = load_module(ROOT / "native/hevc/podman.py", "codec_wrapper")
    runtime = tmp_path / "lepton/images/rootfs"
    (runtime / "vendor/lib64").mkdir(parents=True)
    (runtime / "vendor/etc").mkdir()
    (runtime / "vendor/lib64/libstagefright_softomx.so").write_bytes(b"runtime ABI")
    xml = runtime / "vendor/etc/media_codecs.xml"
    xml.write_text('<MediaCodecs><Include href="stock.xml" /></MediaCodecs>')
    directory = tmp_path / "game/frameport-codec"
    directory.mkdir(parents=True)
    config = {"lepton": str(tmp_path / "lepton/lepton"), "appid": "123",
              "runtime_sha256": hashlib.sha256(b"runtime ABI").hexdigest()}
    exists = Path.exists
    monkeypatch.setattr(Path, "exists", lambda p: True if p.as_posix() == "/dev/video-dec0" else exists(p))
    args = ["run", "--name", "lepton-steamlaunch-123", "--rootfs", str(runtime) + ":O", "/init"]
    extra = wrapper.mounts(directory, config, args)
    assert [s.replace("\\", "/") for s in extra[:2]] == [
        "--mount", "type=bind,source=/dev/video-dec0,destination=/dev/video-dec0,rw"]
    assert "media_codecs_frameport.xml" in (directory / "media_codecs.xml").read_text()
    assert "frameport" not in xml.read_text()  # shared runtime stays unchanged
    assert wrapper.mounts(directory, config, ["kill", "lepton-steamlaunch-123"]) == []
    assert wrapper.mounts(directory, config, [s.replace("123", "456") for s in args]) == []
    config["runtime_sha256"] = "wrong ABI"
    assert wrapper.mounts(directory, config, args) == []


def test_agent_extracts_only_verified_assets_and_removes_old_wrapper(tmp_path, monkeypatch):
    # The agent targets Linux; this test only exercises stdlib ZIP/file work.
    if sys.platform == "win32":
        monkeypatch.setitem(sys.modules, "fcntl", types.SimpleNamespace())
    agent = load_module(ROOT / "agent/frameport_agent.py", "codec_agent")
    monkeypatch.setattr(agent.shutil, "which", lambda _: "/usr/bin/podman")
    base = tmp_path / "game"
    (base / "lepton-app/obb").mkdir(parents=True)
    video = base / "lepton-app/obb/movie.mp4"
    video.write_bytes(b"original large asset")
    apk = base / "lepton-app/game.apk"
    assets = ROOT / "artifacts/hevc"
    with zipfile.ZipFile(apk, "w") as z:
        for name in ("manifest.json", "podman.py", "libstagefrighthw.so", "media_codecs_frameport.xml",
                     "COPYING.FFmpeg"):
            z.write(assets / (name + ".txt" if name == "podman.py" else name), "assets/frameport/hevc/" + name)
    replace = agent.os.replace
    published = []

    def publish(source, target):
        target = Path(target)
        if target.name == "podman":
            config = json.loads((base / "frameport-codec/deployment.json").read_text())
            assert config["podman"] == "/usr/bin/podman"
            assert all((base / "frameport-codec" / n).is_file()
                       for n in ("libstagefrighthw.so", "media_codecs_frameport.xml", "COPYING.FFmpeg"))
        published.append(target.name)
        replace(source, target)

    monkeypatch.setattr(agent.os, "replace", publish)
    assert agent.install_video_codec(str(base), "/lepton/lepton", 123)
    assert published[0] == "deployment.json" and published[-1] == "podman"
    wrapper = base / "frameport-codec/bin/podman"
    assert wrapper.read_bytes() == (assets / "podman.py.txt").read_bytes()
    assert video.read_bytes() == b"original large asset"
    with zipfile.ZipFile(apk, "w") as z:
        z.writestr("AndroidManifest.xml", b"old APK")
    assert not agent.install_video_codec(str(base), "/lepton/lepton", 123)
    assert not wrapper.exists()
    with zipfile.ZipFile(apk, "w") as z:
        z.write(assets / "manifest.json", "assets/frameport/hevc/manifest.json")
        z.writestr("assets/frameport/hevc/libstagefrighthw.so", b"corrupt")
    with pytest.raises(agent.AgentError, match="checksum mismatch"):
        agent.install_video_codec(str(base), "/lepton/lepton", 123)


@pytest.mark.parametrize("configuration", [None, "{", "[]", "{}", '{"podman":null}',
                                          '{"podman":"missing-podman"}', "self"])
def test_broken_wrapper_configuration_executes_stock_podman(tmp_path, monkeypatch, configuration):
    wrapper = load_module(ROOT / "native/hevc/podman.py", "fallback_wrapper")
    directory = tmp_path / "codec"
    own = directory / "bin/podman"
    own.parent.mkdir(parents=True)
    own.write_text("wrapper")
    own.chmod(0o755)
    real = tmp_path / "system/podman"
    real.parent.mkdir()
    real.write_text("stock podman")
    real.chmod(0o755)
    monkeypatch.setattr(wrapper, "__file__", str(own))
    monkeypatch.setenv("PATH", str(own.parent) + os.pathsep + str(real.parent))
    # Avoid Windows' executable-extension rules: this launcher runs on Linux.
    monkeypatch.setattr(wrapper.shutil, "which", lambda _, path: str(Path(path) / "podman"))
    if configuration == "self":
        configuration = json.dumps({"podman": str(own)})
    if configuration is not None:
        (directory / "deployment.json").write_text(configuration)
    args = ["run", "--name", "lepton-steamlaunch-123", "/init"]
    monkeypatch.setattr(wrapper.sys, "argv", [str(own), *args])
    executed = []

    class ExecSucceeded(BaseException):
        pass

    def execute(path, argv):
        if Path(path) != real:
            raise FileNotFoundError(path)
        executed.append((path, argv))
        raise ExecSucceeded

    monkeypatch.setattr(wrapper.os, "execv", execute)
    with pytest.raises(ExecSucceeded):
        wrapper.main()
    assert executed == [(str(real), [str(real), *args])]


def test_failed_codec_mount_or_exec_preserves_original_arguments(tmp_path, monkeypatch):
    wrapper = load_module(ROOT / "native/hevc/podman.py", "mount_failure_wrapper")
    directory = tmp_path / "codec"
    directory.mkdir()
    own = directory / "bin/podman"
    monkeypatch.setattr(wrapper, "__file__", str(own))
    monkeypatch.setattr(wrapper, "real_podman", lambda _: "/usr/bin/podman")
    (directory / "deployment.json").write_text('{"podman":"/missing/podman"}')
    args = ["run", "--name", "lepton-steamlaunch-123", "/init"]
    monkeypatch.setattr(wrapper.sys, "argv", [str(own), *args])
    executed = []

    class ExecSucceeded(BaseException):
        pass

    def execute(path, argv):
        if path != "/usr/bin/podman":
            raise OSError("configured executable is unavailable")
        executed.append(argv)
        raise ExecSucceeded

    monkeypatch.setattr(wrapper.os, "execv", execute)
    monkeypatch.setattr(wrapper, "mounts", lambda *_: ["--mount", "private-codec"])
    with pytest.raises(ExecSucceeded):
        wrapper.main()
    assert executed == [["/usr/bin/podman", *args]]

    def malformed(*_):
        raise KeyError("runtime_sha256")

    monkeypatch.setattr(wrapper, "mounts", malformed)
    with pytest.raises(ExecSucceeded):
        wrapper.main()
    assert executed == [["/usr/bin/podman", *args]] * 2
