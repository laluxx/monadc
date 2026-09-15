from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]


def read(name: str) -> str:
    return (ROOT / name).read_text(encoding="utf-8")


class CMakeBuildTests(unittest.TestCase):
    def test_cmake_declares_runtime_compiler_and_embedding_targets(self):
        cmake = read("CMakeLists.txt")
        self.assertIn("project(monadc C)", cmake)
        self.assertIn("add_library(monad_runtime STATIC", cmake)
        self.assertIn("add_executable(monad", cmake)
        self.assertIn("add_library(monad_embed_static STATIC", cmake)
        self.assertIn("add_library(monad_embed_shared SHARED", cmake)
        self.assertIn("target_link_libraries(monad PRIVATE", cmake)

    def test_cmake_uses_the_canonical_src_tree(self):
        cmake = read("CMakeLists.txt")
        self.assertIn('set(MONADC_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")', cmake)
        self.assertIn("src/tooling/*.c", cmake)
        self.assertIn("src/qtt/*.c", cmake)
        self.assertIn("src/concurrency/*.c", cmake)
        self.assertIn("src/effects/*.c", cmake)
        self.assertIn("src/embed/embed.c", cmake)
        self.assertNotIn("file(GLOB MONADC_ROOT_SOURCES CONFIGURE_DEPENDS *.c)", cmake)

    def test_cmake_exports_compilation_database_for_clangd(self):
        cmake = read("CMakeLists.txt")
        self.assertIn("CMAKE_EXPORT_COMPILE_COMMANDS ON", cmake)

    def test_cmake_discovers_required_external_dependencies(self):
        cmake = read("CMakeLists.txt")
        self.assertIn("find_package(LLVM REQUIRED CONFIG)", cmake)
        self.assertIn("find_package(Threads REQUIRED)", cmake)
        self.assertIn("find_library(READLINE_LIBRARY", cmake)
        self.assertIn("find_library(GMP_LIBRARY", cmake)
        self.assertIn("find_library(CLANG_LIBRARY", cmake)
        self.assertIn("llvm_map_components_to_libnames", cmake)
        self.assertIn("set(MONADC_LLVM_COMPONENTS core orcjit native passes)", cmake)

    def test_cmake_uses_llvm_config_cmakedir_before_llvm_package_lookup(self):
        cmake = read("CMakeLists.txt")
        llvm_config = cmake.index("find_program(LLVM_CONFIG_EXECUTABLE")
        llvm_cmakedir = cmake.index("LLVM_CONFIG_CMAKEDIR")
        llvm_package = cmake.index("find_package(LLVM REQUIRED CONFIG)")
        self.assertLess(llvm_config, llvm_cmakedir)
        self.assertLess(llvm_cmakedir, llvm_package)
        self.assertIn("COMMAND ${LLVM_CONFIG_EXECUTABLE} --cmakedir", cmake)

    def test_cmake_registers_host_contracts_as_python_modules(self):
        cmake = read("CMakeLists.txt")
        self.assertIn("function(monadc_python_test name module)", cmake)
        self.assertIn("-B -m ${module}", cmake)
        self.assertIn("src.testing.contracts.test_windows_portability", cmake)
        self.assertIn("src.testing.contracts.test_checkout_local_paths", cmake)
        self.assertIn("src.testing.contracts.test_unified_test_entrypoint", cmake)
        self.assertNotIn("tests/test_", cmake)

    def test_ci_builds_and_tests_linux_and_windows(self):
        workflow = read(".github/workflows/ci.yml")
        self.assertIn("ubuntu-latest", workflow)
        self.assertIn("windows-latest", workflow)
        self.assertIn("msys2/setup-msys2@v2", workflow)
        self.assertIn("cmake -S . -B build", workflow)
        self.assertIn("cmake --build build --parallel --verbose", workflow)
        self.assertIn("ctest --test-dir build", workflow)
        self.assertIn("MONAD_BINARY=\"$PWD/build/monad\" ./build/monad test runner", workflow)
        self.assertIn("MONAD_BINARY=\"$PWD/build/monad.exe\" ./build/monad.exe test runner", workflow)
        self.assertIn("checkout_local_paths_contract", workflow)
        self.assertNotIn("tests/test_", workflow)


if __name__ == "__main__":
    unittest.main()
