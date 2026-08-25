"""Public contract for the first modern Vulkan tutorial."""

from pathlib import Path
import os
import re
import struct
import subprocess


ROOT = Path(__file__).resolve().parents[1]
HOW_TO = ROOT / "how_to"


def embedded_words(module: str, name: str) -> list[int]:
    match = re.search(
        rf"define {name} :: \[(\d+) U32\]\s*\n\[(.*?)\]",
        module,
        re.DOTALL,
    )
    assert match, f"missing {name} as a typed contiguous buffer"
    words = [int(token, 16) for token in re.findall(r"0x[0-9a-fA-F]+", match.group(2))]
    assert len(words) == int(match.group(1))
    return words


def test_triangle_is_scene_code_not_vulkan_plumbing() -> None:
    source = (HOW_TO / "Triangle.mon").read_text()
    assert "import Graphics.Vulkan" in source
    assert "vulkan-program" in source
    assert "run-vulkan-2d triangle" in source
    for plumbing in ("VkInstance", "VkSwapchain", "VkPipeline", "calloc", "free"):
        assert plumbing not in source


def test_triangle_shaders_are_embedded_and_valid(tmp_path: Path) -> None:
    module = (HOW_TO / "TriangleShaders.mon").read_text()
    for source_name, array_name in (
        ("Triangle.vert", "triangle-vertex-shader"),
        ("Triangle.frag", "triangle-fragment-shader"),
    ):
        binary = tmp_path / f"{source_name}.spv"
        subprocess.run(
            ["glslc", str(HOW_TO / "shaders" / source_name), "-o", str(binary)],
            check=True,
        )
        subprocess.run(["spirv-val", str(binary)], check=True)
        embedded = b"".join(
            struct.pack("<I", word) for word in embedded_words(module, array_name)
        )
        assert embedded == binary.read_bytes()


def test_triangle_compiles_against_core_renderer(tmp_path: Path) -> None:
    environment = os.environ.copy()
    environment["HOME"] = str(tmp_path / "home")
    (tmp_path / "home").mkdir()
    subprocess.run(
        [str(ROOT / "monad"), "Triangle.mon", "-o", str(tmp_path / "Triangle")],
        cwd=HOW_TO,
        env=environment,
        check=True,
        capture_output=True,
        text=True,
    )
