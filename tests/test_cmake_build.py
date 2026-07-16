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
        self.assertIn("POSITION_INDEPENDENT_CODE ON", cmake)
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
        self.assertIn("set(MONADC_LLVM_COMPONENTS core orcjit native passes)", cmake)
        self.assertIn("COMMAND ${LLVM_CONFIG_EXECUTABLE} --libs ${MONADC_LLVM_COMPONENTS} --system-libs", cmake)

    def test_cmake_uses_llvm_config_cmakedir_before_llvm_package_lookup(self):
        cmake = read("CMakeLists.txt")

        llvm_config = cmake.index("find_program(LLVM_CONFIG_EXECUTABLE")
        llvm_cmakedir = cmake.index("LLVM_CONFIG_CMAKEDIR")
        llvm_package = cmake.index("find_package(LLVM REQUIRED CONFIG)")

        self.assertLess(llvm_config, llvm_cmakedir)
        self.assertLess(llvm_cmakedir, llvm_package)
        self.assertIn("COMMAND ${LLVM_CONFIG_EXECUTABLE} --cmakedir", cmake)
        self.assertIn('list(PREPEND CMAKE_PREFIX_PATH "${LLVM_CONFIG_CMAKEDIR}")', cmake)

    def test_cmake_runs_checkout_local_path_smoke(self):
        cmake = read("CMakeLists.txt")

        self.assertIn("MONADC_CHECKOUT_LOCAL_TESTS", cmake)
        self.assertIn("NAME checkout_local_paths.${checkout_local_test}", cmake)
        self.assertIn("add_test(NAME monad_help COMMAND $<TARGET_FILE:monad> --help)", cmake)
        self.assertIn("add_test(\n  NAME repl_contract", cmake)
        self.assertIn("tests/test_checkout_local_paths.py", cmake)
        self.assertIn("tests/test_repl.py", cmake)
        self.assertIn("test_windows_drive_monad_binary_env_is_already_absolute", cmake)
        self.assertIn("MONAD_BINARY=$<TARGET_FILE:monad>", cmake)
        self.assertIn("CheckoutLocalPathTests.${checkout_local_test}", cmake)

    def test_ci_uses_cmake_on_linux_and_msys2_windows(self):
        workflow = read(".github/workflows/ci.yml")

        self.assertIn("ubuntu-latest", workflow)
        self.assertIn("windows-latest", workflow)
        self.assertIn("msys2/setup-msys2@v2", workflow)
        self.assertIn("cmake -S . -B build", workflow)
        self.assertIn("cmake --build build --parallel --verbose", workflow)
        self.assertIn("tee build/build.log", workflow)
        self.assertIn("build/build.log", workflow)
        self.assertIn("ctest --test-dir build", workflow)
        self.assertIn("tee build/ctest.log", workflow)
        self.assertIn("Test checkout local compiler smoke", workflow)
        self.assertIn("Test checkout local package smoke", workflow)
        self.assertIn("test_checkout_compiler_can_compile_program_without_install_env", workflow)
        self.assertIn("test_package_build_finds_checkout_core_from_project_directory", workflow)
        self.assertIn("ctest-checkout-local-compiler-smoke.log", workflow)
        self.assertIn("ctest-checkout-local-package-smoke.log", workflow)
        self.assertIn("build/ctest-*.log", workflow)
        self.assertIn("Run compiler-facing unified suite", workflow)
        self.assertIn("./build/monad test runner", workflow)
        self.assertIn("./build/monad.exe test runner", workflow)
        self.assertIn("build/monad-test-runner.log", workflow)
        self.assertIn("tests/test_repl.py", read("tests/main.py"))

    def test_ci_runs_explicit_portability_smokes(self):
        workflow = read(".github/workflows/ci.yml")

        self.assertIn('MONAD_BINARY="$PWD/build/monad" ./build/monad test runner', workflow)
        self.assertIn('MONAD_BINARY="$PWD/build/monad.exe" ./build/monad.exe test runner', workflow)
        self.assertIn("windows_portability_contract", workflow)
        self.assertIn("checkout_local_paths", workflow)


if __name__ == "__main__":
    unittest.main()
