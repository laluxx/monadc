"""Public contract for the first modern Vulkan tutorial."""

from pathlib import Path
import os
import re
import struct
import subprocess


ROOT = Path(__file__).resolve().parents[1]
HOW_TO = ROOT / "how_to" / "triangle"


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
    assert 'init-window 960 540 "Monad — Vulkan Triangle"' in source
    assert "load-shader" in source
    assert "load-triangle-shaders" not in source
    assert "until window-should-close?" in source
    assert "begin-drawing" in source
    assert "draw-triangle" in source
    assert "end-drawing" in source
    assert "close-window" in source
    assert not re.search(
        r"(?:window-should-close\?|begin-drawing|draw-triangle|end-drawing|close-window)\s+0",
        source,
    )
    assert "run-vulkan-2d" not in source
    assert "vulkan-program" not in source
    for plumbing in ("VkInstance", "VkSwapchain", "VkPipeline", "calloc", "free"):
        assert plumbing not in source


def test_core_wrapper_starts_at_vulkan_13_and_selects_a_present_queue() -> None:
    source = (ROOT / "core" / "Graphics" / "Vulkan.mon").read_text()
    assert "VK_API_VERSION_1_3" in source
    assert "vkGetPhysicalDeviceSurfaceSupportKHR" in source
    assert "VK_QUEUE_GRAPHICS_BIT" in source
    assert "glfwGetFramebufferSize" in source
    assert "glfwWaitEvents" in source
    assert "vulkan-render-finished  :: [8 VkSemaphore]" in source
    assert "vulkan-surface-formats  :: [64 VkSurfaceFormatKHR]" in source


def test_immediate_facade_never_uses_vulkan_after_failed_initialization() -> None:
    source = (ROOT / "core" / "Graphics" / "Vulkan.mon").read_text()
    assert "finish-init-window" in source
    assert "glfwSetWindowShouldClose window True" in source
    assert re.search(
        r"define load-shader .*?\n(?:.*\n){0,4}\s+if not graphics-ready then -1",
        source,
    )
    assert re.search(
        r"define close-immediate-window.*?\n\s+if not graphics-ready then close-incomplete-window",
        source,
    )


def test_triangle_shaders_are_embedded_and_valid(tmp_path: Path) -> None:
    for source_name, module_name, array_name in (
        ("Triangle.vert", "TriangleVert.mon", "triangle-vertex-shader"),
        ("Triangle.frag", "TriangleFrag.mon", "triangle-fragment-shader"),
    ):
        module = (HOW_TO / "Shaders" / module_name).read_text()
        binary = tmp_path / f"{source_name}.spv"
        subprocess.run(
            ["glslc", str(HOW_TO / "Shaders" / source_name), "-o", str(binary)],
            check=True,
        )
        subprocess.run(["spirv-val", str(binary)], check=True)
        embedded = b"".join(
            struct.pack("<I", word) for word in embedded_words(module, array_name)
        )
        embedded_binary = tmp_path / f"embedded-{source_name}.spv"
        embedded_binary.write_bytes(embedded)
        subprocess.run(["spirv-val", str(embedded_binary)], check=True)


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


def test_triangle_recovers_from_a_truncated_ffi_cache(tmp_path: Path) -> None:
    environment = os.environ.copy()
    environment["HOME"] = str(tmp_path / "home")
    (tmp_path / "home").mkdir()
    command = [
        str(ROOT / "monad"), "Triangle.mon", "-o", str(tmp_path / "Triangle")
    ]
    subprocess.run(
        command, cwd=HOW_TO, env=environment, check=True,
        capture_output=True, text=True,
    )
    caches = list((tmp_path / "home" / ".cache" / "monad").glob("*.ffic"))
    assert caches, "Triangle compilation did not create an FFI cache"
    largest = max(caches, key=lambda path: path.stat().st_size)
    largest.write_bytes(largest.read_bytes()[:32])

    recovered = subprocess.run(
        command, cwd=HOW_TO, env=environment, check=False,
        capture_output=True, text=True,
    )
    assert recovered.returncode == 0, recovered.stderr[-4000:]
