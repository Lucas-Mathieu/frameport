#!/usr/bin/python3
"""Add a per-game codec to Lepton's container, without editing its shared rootfs."""
import hashlib
import json
import os
import shutil
import sys
from pathlib import Path
from xml.etree import ElementTree as ET


def mounts(directory, config, args):
    if not args or args[0] != "run":
        return []
    name = None
    for i, arg in enumerate(args):
        if arg == "--name" and i + 1 < len(args):
            name = args[i + 1]
        elif arg.startswith("--name="):
            name = arg.partition("=")[2]
    if name != f"lepton-steamlaunch-{config['appid']}":
        return []
    root = Path(config["lepton"]).resolve().parent / "images" / "rootfs"
    expected_root = str(root) + ":O"
    if not any(arg == expected_root for arg in args):
        return []
    device = Path("/dev/video-dec0")
    runtime = root / "vendor/lib64/libstagefright_softomx.so"
    upstream_plugin = (root / "vendor/lib64/libstagefrighthw.so").exists()
    for i, arg in enumerate(args):
        if arg == "--mount" and i + 1 < len(args):
            fields = dict(item.split("=", 1) for item in args[i + 1].split(",") if "=" in item)
            target = fields.get("destination", fields.get("target"))
            if target == "/vendor/lib64/libstagefrighthw.so":
                upstream_plugin = True
            if target == "/vendor/lib64/libstagefright_softomx.so" and fields.get("source"):
                runtime = Path(fields["source"])
    if upstream_plugin:
        print("FramePort HEVC: using the runtime's hardware codec plugin", file=sys.stderr)
        return []
    if not device.exists() or hashlib.sha256(runtime.read_bytes()).hexdigest() != config["runtime_sha256"]:
        print("FramePort HEVC: device or runtime ABI differs; retaining the stock codecs", file=sys.stderr)
        return []
    xml = ET.parse(root / "vendor/etc/media_codecs.xml")
    ET.SubElement(xml.getroot(), "Include", href="media_codecs_frameport.xml")
    merged = directory / "media_codecs.xml"
    temporary = merged.with_suffix(".tmp")
    xml.write(temporary, encoding="utf-8", xml_declaration=True)
    temporary.replace(merged)
    # Lepton supplies its own /dev tmpfs. A Podman --device node disappears
    # beneath it; a bind mount matches Lepton's existing GPU/sound device setup.
    result = ["--mount", f"type=bind,source={device},destination=/dev/video-dec0,rw"]
    for source, target in (
        ("libstagefrighthw.so", "/vendor/lib64/libstagefrighthw.so"),
        ("media_codecs.xml", "/vendor/etc/media_codecs.xml"),
        ("media_codecs_frameport.xml", "/vendor/etc/media_codecs_frameport.xml"),
    ):
        result += ["--mount", f"type=bind,source={directory / source},destination={target},ro"]
    print("FramePort HEVC: loading the Iris hardware codec plugin for this container", file=sys.stderr)
    return result


def real_podman(directory):
    """Find Podman independently of deployment.json, without recursing into this wrapper."""
    own_bin = (directory / "bin").resolve()
    own_script = Path(__file__).resolve()
    # Lepton runs its `podman exec` calls (boot wait, app pid, logcat mirror) with the Android guest's PATH
    # (/product/bin:/system/bin:...), which has no host Podman: the system folders come after PATH. Failing there
    # broke Lepton's logcat mirror and app-pid checks, and the container was stopped early.
    entries = os.environ.get("PATH", os.defpath).split(os.pathsep) + ["/usr/local/bin", "/usr/bin", "/bin"]
    for entry in entries:
        folder = Path(entry or os.curdir).resolve()
        if folder == own_bin:
            continue
        found = shutil.which("podman", path=str(folder))
        if found:
            executable = Path(found).resolve()
            if executable.parent != own_bin and executable != own_script:
                return str(executable)
    raise RuntimeError("no real Podman executable found outside the codec wrapper directory")


def main():
    directory = Path(__file__).resolve().parent.parent
    args = sys.argv[1:]
    fallback = real_podman(directory)
    try:
        config = json.loads((directory / "deployment.json").read_text())
        podman = Path(config["podman"]).resolve()
        if podman.parent == (directory / "bin").resolve() or podman == Path(__file__).resolve():
            raise ValueError("configured Podman points to the codec wrapper")
        extra = mounts(directory, config, args)
        launch_args = [args[0], *extra, *args[1:]] if extra else args
        os.execv(str(podman), [str(podman), *launch_args])
    except Exception as exc:  # a codec/configuration failure must never prevent the stock container from starting
        print(f"FramePort HEVC: retaining stock codecs: {exc}", file=sys.stderr)
    os.execv(fallback, [fallback, *args])


if __name__ == "__main__":
    main()
