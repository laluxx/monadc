from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read(name: str) -> str:
    return (ROOT / name).read_text(encoding="utf-8")


class CMakeBuildTests(unittest.TestCase):
    def test_cmake_declares_runtime_archive_and_compiler_targets(self):
        cmake = read("CMakeLists.txt")

        self.assertIn("project(monadc C)", cmake)
        self.assertIn("add_library(monad_runtime STATIC", cmake)
        self.assertIn("OUTPUT_NAME monad", cmake)
        self.assertIn("add_executable(monad", cmake)
        self.assertIn("target_link_libraries(monad PRIVATE monad_runtime", cmake)

    def test_cmake_discovers_required_external_dependencies(self):
        cmake = read("CMakeLists.txt")

        self.assertIn("find_package(LLVM REQUIRED CONFIG)", cmake)
        self.assertIn("find_package(Threads REQUIRED)", cmake)
        self.assertIn("NAMES llvm-config llvm-config-20 llvm-config-19 llvm-config-18", cmake)
        self.assertIn("find_library(READLINE_LIBRARY", cmake)
        self.assertIn("find_library(GMP_LIBRARY", cmake)
        self.assertIn("find_library(CLANG_LIBRARY", cmake)
        self.assertIn("HINTS ${LLVM_LIBRARY_DIRS} ${LIBCLANG_LIBRARY_DIRS}", cmake)
        self.assertIn("llvm_map_components_to_libnames", cmake)

    def test_cmake_runs_checkout_local_path_smoke(self):
        cmake = read("CMakeLists.txt")

        self.assertIn("NAME checkout_local_paths", cmake)
        self.assertIn("tests/test_checkout_local_paths.py", cmake)
        self.assertIn("MONAD_BINARY=$<TARGET_FILE:monad>", cmake)

    def test_ci_uses_cmake_on_linux_and_msys2_windows(self):
        workflow = read(".github/workflows/ci.yml")

        self.assertIn("ubuntu-latest", workflow)
        self.assertIn("windows-latest", workflow)
        self.assertIn("msys2/setup-msys2@v2", workflow)
        self.assertIn("cmake -S . -B build", workflow)
        self.assertIn("cmake --build build", workflow)
        self.assertIn("ctest --test-dir build", workflow)

    def test_ci_runs_explicit_portability_smokes(self):
        workflow = read(".github/workflows/ci.yml")

        self.assertIn("python tests/test_windows_portability.py", workflow)
        self.assertIn("MONAD_BINARY=build/monad python tests/test_checkout_local_paths.py", workflow)
        self.assertIn("MONAD_BINARY=build/monad.exe python tests/test_checkout_local_paths.py", workflow)


if __name__ == "__main__":
    unittest.main()
