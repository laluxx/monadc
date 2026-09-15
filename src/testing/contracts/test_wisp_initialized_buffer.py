"""Regression for readable initialized contiguous-buffer declarations."""

from pathlib import Path
import subprocess


from src.testing.monad_binary import resolve_monad_binary
ROOT = Path(__file__).resolve().parents[3]
FIXTURE = ROOT / "tests" / "wisp_initialized_buffer.mon"


def test_initialized_buffer_compiles_and_runs(tmp_path: Path) -> None:
    executable = tmp_path / "wisp-initialized-buffer"
    subprocess.run(
        [str(resolve_monad_binary()), str(FIXTURE), "-o", str(executable)],
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
