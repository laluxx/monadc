import os
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
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
        ROOT / "build" / "bin" / "monad",
        ROOT / "build" / "bin" / "monad.exe",
        ROOT / "build" / "vendor" / "run",
        ROOT / "build" / "monad",       # CMake compatibility
        ROOT / "build" / "monad.exe",   # CMake compatibility
    ):
        if candidate.exists():
            return candidate
    return ROOT / "build" / "bin" / "monad"


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
        ROOT / "build" / "lib" / "libmonad.a",
        ROOT / "build" / "libmonad.a",  # CMake compatibility
    ):
        if candidate.exists():
            return candidate
    return binary.parent / "libmonad.a"
