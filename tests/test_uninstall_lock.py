"""Uninstall from the running app (Reddit 2026-10-12: "Uninstall also doesn't work due to gui.lock"): the app gives up
its own lock first, and a file that can't be deleted is reported instead of stopping the uninstall."""
from pathlib import Path

from frameport import uninstall
from frameport.core import instance


def test_release_lets_the_lock_file_go(tmp_path):
    lock = tmp_path / "gui.lock"
    assert instance.acquire(lock)
    instance.release()
    assert not lock.exists()
    instance.release()  # nothing held: no error
    assert instance.acquire(lock)  # and it can be taken again
    instance.release()


def test_remove_data_dir_reports_files_in_use(tmp_path, monkeypatch):
    data = tmp_path / "FramePort"
    (data / "logs").mkdir(parents=True)
    (data / "library.json").write_text("{}")
    (data / "busy.txt").write_text("x")
    real = Path.unlink

    def unlink(self, missing_ok=False):
        if self.name == "busy.txt":
            raise PermissionError(13, "in use")
        return real(self, missing_ok=missing_ok)

    monkeypatch.setattr(Path, "unlink", unlink)
    monkeypatch.delenv("FRAMEPORT_HOME", raising=False)
    assert uninstall.remove_data_dir(data) == ["busy.txt"]
    assert not (data / "logs").exists() and not (data / "library.json").exists()
