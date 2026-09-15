cmake_minimum_required(VERSION 3.24)
project(monadc C)

include(GNUInstallDirs)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
# clangd/clang-tidy should work from any nested source directory after configure.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Export compile_commands.json" FORCE)

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
find_library(CLANG_LIBRARY NAMES clang libclang HINTS ${LLVM_LIBRARY_DIRS} ${LIBCLANG_LIBRARY_DIRS} REQUIRED)

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

set(MONADC_WARNING_FLAGS -Wall -Wextra)
set(MONADC_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
# Keep project-private quoted headers out of the angle-bracket search path.
# Monad has a features.h; using -Isrc would shadow libc <features.h>.
set(MONADC_PRIVATE_INCLUDE_FLAGS
  "-iquote${MONADC_SOURCE_DIR}"
  "-iquote${MONADC_SOURCE_DIR}/tooling"
)
set(MONADC_INCLUDE_DIRS
  "${MONADC_SOURCE_DIR}/embed/include"
  ${LLVM_INCLUDE_DIRS}
  ${LIBCLANG_INCLUDE_DIRS}
)

set(MONADC_RUNTIME_SOURCES
  "${MONADC_SOURCE_DIR}/arena.c"
  "${MONADC_SOURCE_DIR}/runtime.c"
  "${MONADC_SOURCE_DIR}/runtime_errors.c"
)
file(GLOB MONADC_ROOT_SOURCES CONFIGURE_DEPENDS "${MONADC_SOURCE_DIR}/*.c")
file(GLOB MONADC_QTT_SOURCES CONFIGURE_DEPENDS src/qtt/*.c)
file(GLOB MONADC_CONCURRENCY_SOURCES CONFIGURE_DEPENDS src/concurrency/*.c)
file(GLOB MONADC_EFFECT_SOURCES CONFIGURE_DEPENDS src/effects/*.c)
file(GLOB MONADC_TOOLING_SOURCES CONFIGURE_DEPENDS src/tooling/*.c)
set(MONADC_COMPILER_SOURCES
  ${MONADC_ROOT_SOURCES}
  ${MONADC_QTT_SOURCES}
  ${MONADC_CONCURRENCY_SOURCES}
  ${MONADC_EFFECT_SOURCES}
  ${MONADC_TOOLING_SOURCES}
)
list(REMOVE_ITEM MONADC_COMPILER_SOURCES ${MONADC_RUNTIME_SOURCES})
if(WIN32)
  list(REMOVE_ITEM MONADC_COMPILER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/debugger.c" src/debugger.c)
endif()

add_library(monad_runtime STATIC ${MONADC_RUNTIME_SOURCES})
set_target_properties(monad_runtime PROPERTIES OUTPUT_NAME monad POSITION_INDEPENDENT_CODE ON)
target_compile_options(monad_runtime PRIVATE ${MONADC_WARNING_FLAGS} ${MONADC_PRIVATE_INCLUDE_FLAGS} ${LLVM_DEFINITIONS_LIST})
target_include_directories(monad_runtime PRIVATE ${MONADC_INCLUDE_DIRS})

add_executable(monad ${MONADC_COMPILER_SOURCES})
target_compile_options(monad PRIVATE ${MONADC_WARNING_FLAGS} ${MONADC_PRIVATE_INCLUDE_FLAGS} ${LLVM_DEFINITIONS_LIST})
target_include_directories(monad PRIVATE ${MONADC_INCLUDE_DIRS})
target_link_options(monad PRIVATE ${LLVM_CONFIG_LDFLAGS_LIST})
if(WIN32)
  target_link_options(monad PRIVATE "-Wl,--stack,1073741824")
else()
  target_link_options(monad PRIVATE -rdynamic)
endif()
target_link_libraries(monad PRIVATE
  monad_runtime
  ${READLINE_LIBRARY}
  ${GMP_LIBRARY}
  ${CLANG_LIBRARY}
  ${LLVM_CONFIG_LIBS_LIST}
  Threads::Threads
  m
)

# Small public embedding surface. The full compiler embedding libraries remain
# owned by the Makefile, which is the canonical release build.
add_library(monad_embed_static STATIC src/embed/embed.c src/embed/monad.c)
set_target_properties(monad_embed_static PROPERTIES OUTPUT_NAME monad-embed POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(monad_embed_static PRIVATE MONAD_EMBED_BUILD)
target_compile_options(monad_embed_static PRIVATE ${MONADC_WARNING_FLAGS} ${MONADC_PRIVATE_INCLUDE_FLAGS} -fvisibility=hidden)
target_include_directories(monad_embed_static PRIVATE ${MONADC_INCLUDE_DIRS})

add_library(monad_embed_shared SHARED src/embed/embed.c src/embed/monad.c)
set_target_properties(monad_embed_shared PROPERTIES OUTPUT_NAME monad-embed POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(monad_embed_shared PRIVATE MONAD_EMBED_BUILD)
target_compile_options(monad_embed_shared PRIVATE ${MONADC_WARNING_FLAGS} ${MONADC_PRIVATE_INCLUDE_FLAGS} -fvisibility=hidden)
target_include_directories(monad_embed_shared PRIVATE ${MONADC_INCLUDE_DIRS})
target_link_libraries(monad_embed_shared PRIVATE Threads::Threads)

enable_testing()
add_executable(tooling_cst_test
  src/testing/contracts/fixtures/tooling_cst_test.c
  src/tooling/cst.c
)
target_include_directories(tooling_cst_test PRIVATE src/tooling)
target_compile_options(tooling_cst_test PRIVATE ${MONADC_WARNING_FLAGS})
add_test(NAME tooling_cst_roundtrip COMMAND tooling_cst_test)

add_executable(tooling_rewrite_test
  src/testing/contracts/fixtures/tooling_rewrite_test.c
  src/tooling/diagnostic.c
  src/tooling/rewrite.c
)
target_include_directories(tooling_rewrite_test PRIVATE src/tooling)
target_compile_options(tooling_rewrite_test PRIVATE ${MONADC_WARNING_FLAGS})
add_test(NAME tooling_checked_rewrite COMMAND tooling_rewrite_test)

function(monadc_python_test name module)
  add_test(NAME ${name} COMMAND ${Python3_EXECUTABLE} -B -m ${module})
  set_tests_properties(${name} PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    ENVIRONMENT "MONAD_BINARY=$<TARGET_FILE:monad>"
  )
endfunction()

add_test(NAME monad_help COMMAND $<TARGET_FILE:monad> --help)
add_test(NAME monad_test_suite_menu COMMAND $<TARGET_FILE:monad> test list)
set_tests_properties(monad_test_suite_menu PROPERTIES
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  ENVIRONMENT "MONAD_BINARY=$<TARGET_FILE:monad>"
)
monadc_python_test(windows_portability_contract src.testing.contracts.test_windows_portability)
monadc_python_test(cmake_build_contract src.testing.contracts.test_cmake_build)
monadc_python_test(unified_test_entrypoint_contract src.testing.contracts.test_unified_test_entrypoint)
monadc_python_test(checkout_local_paths_contract src.testing.contracts.test_checkout_local_paths)
monadc_python_test(repl_contract src.testing.contracts.test_repl)
monadc_python_test(how_to_examples src.testing.contracts.test_how_to_examples)
monadc_python_test(core_zero_cost_contract src.testing.contracts.test_core_zero_cost)
monadc_python_test(anonymous_finite_type_contract src.testing.contracts.test_anonymous_finite_types)

install(TARGETS monad monad_runtime monad_embed_static monad_embed_shared
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES src/runtime.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/monad)
install(DIRECTORY src/embed/include/monad/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/monad FILES_MATCHING PATTERN "*.h")
install(DIRECTORY core/ DESTINATION ${CMAKE_INSTALL_LIBDIR}/monad/core FILES_MATCHING PATTERN "*.mon")
