"""Regression coverage for the Vulkan Blood Moon shader boundary."""

from pathlib import Path
import os
import re
import shutil
import struct
import subprocess


from src.testing.monad_binary import resolve_monad_binary
ROOT = Path(__file__).resolve().parents[3]
EXAMPLE = ROOT / "how_to" / "BloodMoon"


def compile_shader(source: Path, target: Path) -> None:
    subprocess.run(
        ["glslc", str(source), "-o", str(target)],
        check=True,
        capture_output=True,
        text=True,
    )
    subprocess.run(
        ["spirv-val", "--target-env", "vulkan1.0", str(target)],
        check=True,
        capture_output=True,
        text=True,
    )


def embedded_words(module: str, name: str) -> list[int]:
    match = re.search(
        rf"define {name} :: \[(\d+) U32\]\s*\n\[(.*?)\]",
        module,
        re.DOTALL,
    )
    assert match, f"missing typed contiguous shader array {name}"
    words = [int(token, 16) for token in re.findall(r"0x[0-9a-fA-F]+", match.group(2))]
    assert len(words) == int(match.group(1))
    return words


def test_embedded_shaders_are_current_and_valid(tmp_path: Path) -> None:
    assert shutil.which("glslc")
    assert shutil.which("spirv-val")

    module = (EXAMPLE / "Shaders.mon").read_text()
    for source_name, array_name in (
        ("fullscreen.vert", "fullscreen-vertex-shader"),
        ("blood.frag", "blood-fragment-shader"),
    ):
        binary = tmp_path / f"{source_name}.spv"
        compile_shader(EXAMPLE / "shaders" / source_name, binary)
        words = embedded_words(module, array_name)
        embedded = b"".join(struct.pack("<I", word) for word in words)
        assert embedded == binary.read_bytes()


def test_shader_module_compiles() -> None:
    subprocess.run(
        [str(resolve_monad_binary()), "Shaders.mon"],
        cwd=EXAMPLE,
        check=True,
        capture_output=True,
        text=True,
    )


def test_scene_module_uses_core_vulkan_boundary(tmp_path: Path) -> None:
    environment = os.environ.copy()
    environment["HOME"] = str(tmp_path / "home")
    (tmp_path / "home").mkdir()
    subprocess.run(
        [str(resolve_monad_binary()), "BloodMoon.mon"],
        cwd=EXAMPLE,
        env=environment,
        check=True,
        capture_output=True,
        text=True,
    )
