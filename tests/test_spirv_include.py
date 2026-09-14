"""The compiler CLI turns a shader into an ordinary typed Monad module."""

from pathlib import Path
import re
import struct
import subprocess


ROOT = Path(__file__).resolve().parents[1]
SHADER = ROOT / "tests" / "spirv_include.vert"


def test_spirv_command_emits_a_valid_monad_module(tmp_path: Path) -> None:
    output = tmp_path / "TriangleVertex.mon"
    subprocess.run(
        [
            str(ROOT / "monad"),
            "spirv",
            str(SHADER),
            "-o",
            str(output),
            "--name",
            "triangle-vertex-shader",
        ],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    )

    module = output.read_text()
    assert "module TriangleVertex" in module
    assert "define triangle-vertex-shader-size :: Int" in module
    match = re.search(
        r"define triangle-vertex-shader :: \[(\d+) U32\]\s*\n\[(.*?)\]",
        module,
        re.DOTALL,
    )
    assert match
    words = [int(word, 16) for word in re.findall(r"0x[0-9a-fA-F]+", match.group(2))]
    assert len(words) == int(match.group(1))

    binary = tmp_path / "reference.spv"
    subprocess.run(
        ["glslc", "--target-env=vulkan1.3", str(SHADER), "-o", str(binary)],
        check=True,
    )
    subprocess.run(
        ["spirv-val", "--target-env", "vulkan1.3", str(binary)], check=True
    )
    embedded = b"".join(struct.pack("<I", word) for word in words)
    assert embedded == binary.read_bytes()
    assert f"define triangle-vertex-shader-size :: Int {len(embedded)}\n" in module

    subprocess.run(
        [str(ROOT / "monad"), str(output), "-o", str(tmp_path / "shader-module")],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    )


def test_spirv_command_reports_missing_shader(tmp_path: Path) -> None:
    result = subprocess.run(
        [
            str(ROOT / "monad"),
            "spirv",
            str(tmp_path / "does-not-exist.frag"),
            "-o",
            str(tmp_path / "Missing.mon"),
            "--name",
            "missing-shader",
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    assert result.returncode != 0
    assert "does-not-exist.frag" in result.stderr


def test_spirv_command_requires_a_monad_output(tmp_path: Path) -> None:
    result = subprocess.run(
        [str(ROOT / "monad"), "spirv", str(SHADER), "-o", str(tmp_path / "bad.c")],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    assert result.returncode != 0
    assert ".mon" in result.stderr


def test_spirv_command_derives_binding_name_from_shader(tmp_path: Path) -> None:
    output = tmp_path / "Shader.mon"
    subprocess.run(
        [str(ROOT / "monad"), "spirv", str(SHADER), "-o", str(output)],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    )
    module = output.read_text()
    assert "define spirv_include-size :: Int" in module
    assert "define spirv_include :: [" in module


def test_spirv_command_derives_output_module_when_omitted(tmp_path: Path) -> None:
    shader = tmp_path / "blood.frag"
    shader.write_text(
        "#version 450\nlayout(location = 0) out vec4 color;\n"
        "void main() { color = vec4(0.5, 0.0, 0.0, 1.0); }\n"
    )

    subprocess.run(
        [str(ROOT / "monad"), "spirv", "blood.frag"],
        cwd=tmp_path,
        check=True,
        capture_output=True,
        text=True,
    )

    module = (tmp_path / "BloodFrag.mon").read_text()
    assert "module BloodFrag" in module
    assert "define blood-size :: Int" in module
    assert "define blood :: [" in module


def test_spirv_command_fragment_and_vertex_outputs_never_collide(
    tmp_path: Path,
) -> None:
    (tmp_path / "Triangle.frag").write_text(
        "#version 450\nlayout(location = 0) out vec4 color;\n"
        "void main() { color = vec4(1.0); }\n"
    )
    (tmp_path / "Triangle.vert").write_text(
        "#version 450\nvoid main() { gl_Position = vec4(0.0); }\n"
    )

    subprocess.run(
        [str(ROOT / "monad"), "spirv", "Triangle.frag"],
        cwd=tmp_path,
        check=True,
        capture_output=True,
        text=True,
    )
    subprocess.run(
        [str(ROOT / "monad"), "spirv", "Triangle.vert"],
        cwd=tmp_path,
        check=True,
        capture_output=True,
        text=True,
    )

    fragment = (tmp_path / "TriangleFrag.mon").read_text()
    vertex = (tmp_path / "TriangleVert.mon").read_text()
    assert "module TriangleFrag" in fragment
    assert "module TriangleVert" in vertex
    assert ";;; TriangleFrag.mon --- SPIR-V embedded from Triangle.frag" in fragment
    assert ";;; TriangleVert.mon --- SPIR-V embedded from Triangle.vert" in vertex
    assert not (tmp_path / "Triangle.mon").exists()
