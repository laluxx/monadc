"""Contiguous fixed foreign-layout values for native aggregate APIs."""

from pathlib import Path
import os
import shutil
import subprocess


from src.testing.monad_binary import resolve_monad_binary
ROOT = Path(__file__).resolve().parents[3]


def test_initialized_foreign_layout_buffer_is_inline_and_indexable(tmp_path: Path) -> None:
    executable = tmp_path / "foreign-layout-buffer"
    source = tmp_path / "foreign-layout-buffer.mon"
    shutil.copy(ROOT / "tests" / "wisp_foreign_layout_buffer.mon", source)
    environment = os.environ.copy()
    environment["HOME"] = str(tmp_path / "home")
    (tmp_path / "home").mkdir()
    subprocess.run(
        [
            str(resolve_monad_binary()),
            str(source),
            "--emit-ir",
            "-o",
            str(executable),
        ],
        cwd=ROOT,
        env=environment,
        check=True,
        capture_output=True,
        text=True,
    )
    result = subprocess.run(
        [str(executable)], check=True, capture_output=True, text=True
    )
    assert result.stdout == "2\n1\n1\n640\n1\n"

    ir = source.with_suffix(".ll").read_text()
    assert "@stages = global [2 x %layout.VkPipelineShaderStageCreateInfo]" in ir
    assert "[2 x ptr]" not in ir
    reset_body = ir.split("define i32 @reset-selected-command()", 1)[1].split("}", 1)[0]
    assert reset_body.count("load ptr") == 1
