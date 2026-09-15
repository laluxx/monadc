"""Runtime regressions for the interactive lambda-calculus example."""

from pathlib import Path
import os
import subprocess


from src.testing.monad_binary import resolve_monad_binary
ROOT = Path(__file__).resolve().parents[3]
HOW_TO = ROOT / "how_to"


def test_abstraction_and_identity_application_do_not_crash(tmp_path: Path) -> None:
    environment = os.environ.copy()
    environment["HOME"] = str(tmp_path / "home")
    (tmp_path / "home").mkdir()
    executable = tmp_path / "LambdaCalculus"

    subprocess.run(
        [str(resolve_monad_binary()), "LambdaCalculus.mon", "-o", str(executable)],
        cwd=HOW_TO,
        env=environment,
        check=True,
        capture_output=True,
        text=True,
    )

    completed = subprocess.run(
        [str(executable)],
        input="\\x. x 4\n(\\x. x) 4\n:quit\n",
        cwd=HOW_TO,
        check=False,
        capture_output=True,
        text=True,
    )

    assert completed.returncode == 0, completed.stderr
    assert "λx. x 4" in completed.stdout
    assert "λ> 4\n" in completed.stdout
