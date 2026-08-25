"""Public compile/run boundary for Core's Vulkan ownership layer."""

from pathlib import Path
import os
import subprocess


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests" / "core_graphics_vulkan.mon"


def test_core_vulkan_module_imports_and_runs(tmp_path: Path) -> None:
    executable = tmp_path / "core-graphics-vulkan"
    environment = os.environ.copy()
    environment["HOME"] = str(tmp_path / "home")
    (tmp_path / "home").mkdir()
    subprocess.run(
        [str(ROOT / "monad"), str(FIXTURE), "-o", str(executable)],
        cwd=ROOT,
        env=environment,
        check=True,
        capture_output=True,
        text=True,
    )
    result = subprocess.run(
        [str(executable)],
        check=True,
        capture_output=True,
        text=True,
    )
    assert result.stdout == "True\n-7\n"
