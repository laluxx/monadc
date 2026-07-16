cmake_minimum_required(VERSION 3.24)

project(monadc C)

include(GNUInstallDirs)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_program(LLVM_CONFIG_EXECUTABLE
  NAMES llvm-config llvm-config-20 llvm-config-19 llvm-config-18 llvm-config-17 llvm-config-16
  REQUIRED
)
execute_process(
  COMMAND ${LLVM_CONFIG_EXECUTABLE} --cmakedir
  OUTPUT_VARIABLE LLVM_CONFIG_CMAKEDIR
  OUTPUT_STRIP_TRAILING_WHITESPACE
  COMMAND_ERROR_IS_FATAL ANY
)
if(LLVM_CONFIG_CMAKEDIR)
  list(PREPEND CMAKE_PREFIX_PATH "${LLVM_CONFIG_CMAKEDIR}")
endif()

find_package(LLVM REQUIRED CONFIG)
find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

if(PkgConfig_FOUND)
  pkg_check_modules(LIBCLANG QUIET libclang)
endif()

set(MONADC_LLVM_COMPONENTS core orcjit native passes)
llvm_map_components_to_libnames(LLVM_LIBRARIES ${MONADC_LLVM_COMPONENTS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")

find_library(READLINE_LIBRARY NAMES readline REQUIRED)
find_library(GMP_LIBRARY NAMES gmp REQUIRED)
find_library(CLANG_LIBRARY
  NAMES clang libclang
  HINTS ${LLVM_LIBRARY_DIRS} ${LIBCLANG_LIBRARY_DIRS}
  REQUIRED
)

execute_process(
  COMMAND ${LLVM_CONFIG_EXECUTABLE} --ldflags
  OUTPUT_VARIABLE LLVM_CONFIG_LDFLAGS
  OUTPUT_STRIP_TRAILING_WHITESPACE
  COMMAND_ERROR_IS_FATAL ANY
)
execute_process(
  COMMAND ${LLVM_CONFIG_EXECUTABLE} --libs ${MONADC_LLVM_COMPONENTS} --system-libs
  OUTPUT_VARIABLE LLVM_CONFIG_LIBS
  OUTPUT_STRIP_TRAILING_WHITESPACE
  COMMAND_ERROR_IS_FATAL ANY
)
separate_arguments(LLVM_CONFIG_LDFLAGS_LIST UNIX_COMMAND "${LLVM_CONFIG_LDFLAGS}")
separate_arguments(LLVM_CONFIG_LIBS_LIST UNIX_COMMAND "${LLVM_CONFIG_LIBS}")

set(MONADC_WARNING_FLAGS
  -Wall
  -Wextra
)

set(MONADC_RUNTIME_SOURCES
  arena.c
  runtime.c
)

set(MONADC_COMPILER_SOURCES
  asm.c
  buildsystem.c
  bytecode.c
  cli.c
  codegen.c
  completion.c
  config.c
  debugger.c
  dep.c
  env.c
  features.c
  ffi.c
  infer.c
  lsp.c
  lsp_repl.c
  macro.c
  main.c
  module.c
  module_export.c
  optimizations.c
  pmatch.c
  reader.c
  repl.c
  typeclass.c
  types.c
  typst_emit.c
  wisp.c
)

if(WIN32)
  list(REMOVE_ITEM MONADC_COMPILER_SOURCES debugger.c)
endif()

add_library(monad_runtime STATIC ${MONADC_RUNTIME_SOURCES})
set_target_properties(monad_runtime PROPERTIES
  OUTPUT_NAME monad
  POSITION_INDEPENDENT_CODE ON
)
target_compile_options(monad_runtime PRIVATE ${MONADC_WARNING_FLAGS})
target_include_directories(monad_runtime PRIVATE
  ${LLVM_INCLUDE_DIRS}
)
target_compile_options(monad_runtime PRIVATE ${LLVM_DEFINITIONS_LIST})

add_executable(monad ${MONADC_COMPILER_SOURCES})
target_compile_options(monad PRIVATE ${MONADC_WARNING_FLAGS} ${LLVM_DEFINITIONS_LIST})
target_include_directories(monad PRIVATE
  ${LLVM_INCLUDE_DIRS}
  ${LIBCLANG_INCLUDE_DIRS}
)
target_link_options(monad PRIVATE ${LLVM_CONFIG_LDFLAGS_LIST})
if(WIN32)
  # Core loading and dependent inference exceed MinGW's small default stack.
  target_link_options(monad PRIVATE "-Wl,--stack,67108864")
endif()
target_link_libraries(monad PRIVATE monad_runtime
  ${READLINE_LIBRARY}
  ${GMP_LIBRARY}
  ${CLANG_LIBRARY}
  ${LLVM_CONFIG_LIBS_LIST}
  Threads::Threads
  m
)

if(NOT WIN32)
  target_link_options(monad PRIVATE -rdynamic)
endif()

enable_testing()

add_test(NAME monad_help COMMAND $<TARGET_FILE:monad> --help)
add_test(
  NAME windows_portability_contract
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_windows_portability.py
)
add_test(
  NAME cmake_build_contract
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cmake_build.py
)
add_test(
  NAME unified_test_entrypoint_contract
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_unified_test_entrypoint.py
)
add_test(
  NAME repl_contract
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_repl.py
)
add_test(
  NAME monad_test_suite_menu
  COMMAND $<TARGET_FILE:monad> test list
)
add_test(
  NAME how_to_examples
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_how_to_examples.py
)
set(MONADC_CHECKOUT_LOCAL_TESTS
  test_relative_monad_binary_env_resolves_from_checkout_root
  test_windows_drive_monad_binary_env_is_already_absolute
  test_generated_executable_prefers_windows_suffix_when_present
  test_compiler_discovers_checkout_core_and_runtime_archive
  test_legacy_buildsystem_links_runtime_archive_not_object_file
  test_repl_module_import_link_uses_runtime_archive_resolver
  test_checkout_compiler_can_compile_program_without_install_env
  test_compiler_returns_failure_when_linker_fails
  test_package_build_finds_checkout_core_from_project_directory
)

foreach(checkout_local_test IN LISTS MONADC_CHECKOUT_LOCAL_TESTS)
  add_test(
    NAME checkout_local_paths.${checkout_local_test}
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_checkout_local_paths.py
      CheckoutLocalPathTests.${checkout_local_test}
  )
endforeach()

set_tests_properties(
  windows_portability_contract
  cmake_build_contract
  unified_test_entrypoint_contract
  PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

set_tests_properties(
  unified_test_entrypoint_contract
  repl_contract
  monad_test_suite_menu
  PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    ENVIRONMENT "MONAD_BINARY=$<TARGET_FILE:monad>"
)

set_tests_properties(
  how_to_examples
  PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    ENVIRONMENT "MONAD_BINARY=$<TARGET_FILE:monad>"
)

foreach(checkout_local_test IN LISTS MONADC_CHECKOUT_LOCAL_TESTS)
  set_tests_properties(
    checkout_local_paths.${checkout_local_test}
    PROPERTIES
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      ENVIRONMENT "MONAD_BINARY=$<TARGET_FILE:monad>"
  )
endforeach()

install(TARGETS monad monad_runtime
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES runtime.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/monad)
install(DIRECTORY core/ DESTINATION ${CMAKE_INSTALL_LIBDIR}/monad/core
  FILES_MATCHING PATTERN "*.mon"
)
