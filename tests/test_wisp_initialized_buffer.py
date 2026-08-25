"""Regression for readable initialized contiguous-buffer declarations."""

from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests" / "wisp_initialized_buffer.mon"


def test_initialized_buffer_compiles_and_runs(tmp_path: Path) -> None:
    executable = tmp_path / "wisp-initialized-buffer"
    subprocess.run(
        [str(ROOT / "monad"), str(FIXTURE), "-o", str(executable)],
        cwd=ROOT,
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
    assert result.stdout == "4\n119734787\n"
