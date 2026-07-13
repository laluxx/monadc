import os
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
WINDOWS_DRIVE_PATH = re.compile(r"^[A-Za-z]:[\\/]")


def is_absolute_path(value: str | os.PathLike[str]) -> bool:
    raw = str(value)
    return Path(raw).is_absolute() or bool(WINDOWS_DRIVE_PATH.match(raw))


def resolve_monad_binary(value: str | os.PathLike[str] | None = None) -> Path:
    raw = value or os.environ.get("MONAD_BINARY")
    if raw:
        path = Path(raw)
        if not is_absolute_path(raw):
            path = ROOT / path
        return path

    for candidate in (
        ROOT / "monad",
        ROOT / "monad.exe",
        ROOT / "build" / "monad",
        ROOT / "build" / "monad.exe",
    ):
        if candidate.exists():
            return candidate
    return ROOT / "monad"


def generated_executable(path: Path) -> Path:
    exe_path = Path(str(path) + ".exe")
    if exe_path.exists():
        return exe_path
    return path


def resolve_runtime_archive(monad_binary: Path | None = None) -> Path:
    raw = os.environ.get("MONAD_RUNTIME_LIB")
    if raw:
        path = Path(raw)
        if not is_absolute_path(raw):
            path = ROOT / path
        return path

    binary = monad_binary or resolve_monad_binary()
    for candidate in (
        binary.parent / "libmonad.a",
        ROOT / "libmonad.a",
        ROOT / "build" / "libmonad.a",
    ):
        if candidate.exists():
            return candidate
    return binary.parent / "libmonad.a"
