CC          = gcc
PYTHON      = python3
TARGET_BASE = monad
PREFIX  = /usr/local
BINDIR  = $(PREFIX)/bin
LIBDIR  = $(PREFIX)/lib
INCDIR  = $(PREFIX)/include/monad
COREDIR = $(PREFIX)/lib/monad/core

UNAME_S      := $(shell uname -s 2>/dev/null || echo unknown)
WINDOWS_HOST := $(if $(filter MINGW% MSYS% CYGWIN%,$(UNAME_S)),1,)
EXEEXT       := $(if $(WINDOWS_HOST),.exe,)
TARGET       = $(TARGET_BASE)$(EXEEXT)
EXPORT_LDFLAG := $(if $(WINDOWS_HOST),,-rdynamic)
NO_PIE_LDFLAG := $(if $(WINDOWS_HOST),,-no-pie)

CFLAGS  = -Wall -Wextra -std=c99 $(shell llvm-config --cflags)
LLVM_COMPONENTS = core orcjit native passes
LDFLAGS = -lm -lreadline -lpthread -lgmp $(shell llvm-config --ldflags --libs $(LLVM_COMPONENTS)) -lclang

DEBUG_CFLAGS   = -g -DDEBUG
ASAN_CFLAGS    = -g -fsanitize=address -fno-omit-frame-pointer -DDEBUG
RELEASE_CFLAGS = -DNDEBUG -O2

NPROCS = $(shell nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
MAKEFLAGS += -j$(NPROCS)

# Static archive — no rpath/ldconfig needed, works from any directory
RUNTIME_LIB = libmonad.a
RUNTIME_SRC = runtime.c runtime_errors.c arena.c
RUNTIME_OBJ = $(RUNTIME_SRC:.c=.o)
EMBED_STATIC_LIB = libmonad-embed.a
EMBED_SHARED_LIB = libmonad-embed.so
EMBED_OBJ = embed/embed.o embed/monad.o
COMPILER_STATIC_LIB = libmonad-compiler.a
COMPILER_SHARED_LIB = libmonad-compiler.so
COMPILER_FRONTEND_SRC = features.c macro.c pmatch.c reader.c reader_diagnostic.c reader_syntax.c types.c wisp.c wisp_syntax_policy.c
COMPILER_FRONTEND_OBJ = $(patsubst %.c,embed/frontend_%.o,$(COMPILER_FRONTEND_SRC))
COMPILER_INFER_OBJ = embed/compiler_infer.o embed/compiler_infer_support.o embed/compiler_qtt_constraints.o embed/compiler_qtt_effect.o embed/compiler_effect_constraints.o embed/compiler_qtt_environment.o embed/compiler_qtt_quantity.o embed/compiler_qtt_bindings.o embed/compiler_qtt_elaboration.o embed/compiler_qtt_source_grades.o embed/compiler_qtt_anf.o embed/compiler_qtt_core.o embed/compiler_qtt_demand.o embed/compiler_qtt_graded.o embed/compiler_qtt_signature.o embed/compiler_qtt_call.o embed/compiler_qtt_resource.o embed/compiler_qtt_signature_env.o
COMPILER_BACKEND_OBJ = embed/backend_codegen.o embed/backend_env.o embed/backend_typeclass.o embed/backend_language_module.o embed/backend_asm.o embed/backend_ffi.o embed/backend_backend.o embed/backend_compiler.o embed/backend_compiler_module.o embed/backend_core_effect.o embed/backend_interface.o embed/backend_semantic_ir.o embed/backend_effect_runtime.o embed/backend_qtt_module.o embed/backend_drop.o embed/backend_evidence.o embed/backend_closure_policy.o embed/backend_closure.o embed/backend_core_usage.o embed/backend_semantic_anf.o
COMPILER_API_OBJ = embed/compiler.o embed/surface_compiler.o embed/surface_load.o embed/compiler_native.o embed/frontend_transaction.o embed/native_compile.o embed/compiler_qtt_foreign_type.o embed/compiler_qtt_type_identity.o $(COMPILER_INFER_OBJ) $(COMPILER_FRONTEND_OBJ) $(COMPILER_BACKEND_OBJ) $(RUNTIME_OBJ)
HEADERS = $(wildcard *.h qtt/*.h tooling/*.h)

# All compiler .c files except runtime sources and platform-only sources.
WINDOWS_EXCLUDED_SRCS =
ifeq ($(WINDOWS_HOST),1)
WINDOWS_EXCLUDED_SRCS = debugger.c
endif
COMPILER_EXCLUDED_SRCS = $(RUNTIME_SRC) $(WINDOWS_EXCLUDED_SRCS)
SRCS = $(filter-out $(COMPILER_EXCLUDED_SRCS), $(wildcard *.c) $(wildcard qtt/*.c) $(wildcard effects/*.c) $(wildcard tooling/*.c))
FFI_CFLAGS = $(shell pkg-config --cflags libclang 2>/dev/null || echo "-I/usr/lib/llvm/include")
OBJS = $(SRCS:.c=.o)

all: CFLAGS += $(DEBUG_CFLAGS)
all: $(RUNTIME_LIB) $(EMBED_STATIC_LIB) $(EMBED_SHARED_LIB) $(COMPILER_STATIC_LIB) $(COMPILER_SHARED_LIB) $(TARGET)

asan: CFLAGS += $(ASAN_CFLAGS)
asan: LDFLAGS += -fsanitize=address
asan: $(RUNTIME_LIB) $(EMBED_STATIC_LIB) $(EMBED_SHARED_LIB) $(COMPILER_STATIC_LIB) $(COMPILER_SHARED_LIB) $(TARGET)

release: CFLAGS += $(RELEASE_CFLAGS)
release: $(RUNTIME_LIB) $(EMBED_STATIC_LIB) $(EMBED_SHARED_LIB) $(COMPILER_STATIC_LIB) $(COMPILER_SHARED_LIB) $(TARGET)

$(RUNTIME_OBJ): %.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(RUNTIME_LIB): $(RUNTIME_OBJ)
	ar rcs $@ $^

embed/embed.o: embed/embed.c embed/include/monad/embed.h embed/runtime_internal.h
	$(CC) -Wall -Wextra -std=c99 -fPIC -fvisibility=hidden -DMONAD_EMBED_BUILD -c $< -o $@

embed/monad.o: embed/monad.c embed/include/monad/monad.h embed/include/monad/embed.h embed/runtime_internal.h embed/surface_internal.h
	$(CC) -Wall -Wextra -std=c99 -fPIC -fvisibility=hidden -DMONAD_EMBED_BUILD -c $< -o $@

$(EMBED_STATIC_LIB): $(EMBED_OBJ)
	ar rcs $@ $^

$(EMBED_SHARED_LIB): $(EMBED_OBJ)
	$(CC) -shared -o $@ $^ -lpthread

embed/compiler.o: embed/compiler.c embed/include/monad/compiler.h embed/include/monad/embed.h qtt/foreign_type.h
	$(CC) -Wall -Wextra -std=c99 -fPIC -fvisibility=hidden -DMONAD_COMPILER_BUILD -Iembed/include -c $< -o $@

embed/surface_compiler.o: embed/surface_compiler.c embed/include/monad/monad.h embed/include/monad/compiler.h
	$(CC) -Wall -Wextra -std=c99 -fPIC -fvisibility=hidden -DMONAD_COMPILER_BUILD -Iembed/include -c $< -o $@

embed/surface_load.o: embed/surface_load.c embed/include/monad/monad.h embed/compiler_internal.h embed/native_compile.h embed/surface_internal.h
	$(CC) -Wall -Wextra -std=c99 -fPIC -fvisibility=hidden -DMONAD_COMPILER_BUILD -Iembed/include -c $< -o $@

embed/compiler_native.o: embed/compiler_native.c embed/compiler_internal.h embed/native_compile.h
	$(CC) -Wall -Wextra -std=c99 -fPIC -fvisibility=hidden -DMONAD_COMPILER_BUILD -Iembed/include -c $< -o $@

embed/frontend_transaction.o: embed/frontend_transaction.c embed/frontend_transaction.h reader.h reader_diagnostic.h wisp.h
	$(CC) -Wall -Wextra -std=c99 -fPIC -fvisibility=hidden -c $< -o $@

embed/native_compile.o: embed/native_compile.c embed/native_compile.h embed/frontend_transaction.h qtt/core.h
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@

embed/compiler_qtt_foreign_type.o: qtt/foreign_type.c qtt/foreign_type.h
	$(CC) -Wall -Wextra -std=c99 -fPIC -fvisibility=hidden -c $< -o $@

embed/compiler_qtt_type_identity.o: qtt/type_identity.c qtt/type_identity.h
	$(CC) -Wall -Wextra -std=c99 -fPIC -fvisibility=hidden -c $< -o $@

embed/compiler_infer.o: infer.c infer.h
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@

embed/compiler_infer_support.o: embed/infer_support.c
	$(CC) -Wall -Wextra -std=c99 -fPIC -fvisibility=hidden -c $< -o $@

embed/compiler_qtt_effect.o: effects/effect.c effects/effect.h
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@

embed/compiler_qtt_elaboration.o: qtt/elaboration.c qtt/elaboration.h
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@

embed/compiler_qtt_bindings.o: qtt/bindings.c qtt/bindings.h
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@

embed/compiler_qtt_source_grades.o: qtt/pipeline.c qtt/pipeline.h
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@

embed/compiler_qtt_anf.o: qtt/anf.c qtt/anf.h
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@
embed/compiler_qtt_core.o: qtt/core.c qtt/core.h
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@
embed/compiler_qtt_demand.o: qtt/demand.c qtt/demand.h
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@
embed/compiler_qtt_graded.o: qtt/graded.c qtt/graded.h
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@
embed/compiler_qtt_signature.o: qtt/signature.c qtt/signature.h
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@
embed/compiler_qtt_call.o: qtt/call.c qtt/call.h
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@
embed/compiler_qtt_resource.o: qtt/resource.c qtt/resource.h
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@
embed/compiler_qtt_signature_env.o: qtt/signature_env.c qtt/signature_env.h
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@

embed/compiler_effect_constraints.o: effects/constraints.c effects/constraints.h
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@

embed/compiler_qtt_%.o: qtt/%.c
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@

embed/backend_backend.o: qtt/backend.c
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@
embed/backend_compiler.o: qtt/compiler.c
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@
embed/backend_compiler_module.o: qtt/compiler_module.c
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@
embed/backend_core_effect.o: qtt/core_effect.c
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@
embed/backend_interface.o: qtt/interface.c
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@
embed/backend_semantic_ir.o: qtt/semantic_ir.c
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@
embed/backend_effect_runtime.o: qtt/effect_runtime.c
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@
embed/backend_qtt_module.o: qtt/module.c
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@
embed/backend_drop.o: qtt/drop.c
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@
embed/backend_evidence.o: qtt/evidence.c
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@
embed/backend_closure_policy.o: qtt/closure_policy.c
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@
embed/backend_closure.o: qtt/closure.c
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@
embed/backend_core_usage.o: qtt/core_usage.c
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@
embed/backend_semantic_anf.o: qtt/semantic_anf.c
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@
embed/backend_ffi.o: ffi.c
	$(CC) $(CFLAGS) $(FFI_CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@
embed/backend_language_module.o: module.c
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@
embed/backend_%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@

# The embedding compiler owns a distinct PIC copy of the frontend closure.
# Hidden-by-default compilation makes the installed C API the sole shared ABI;
# this follows ELF visibility guidance rather than relying on symbol naming:
# https://gcc.gnu.org/onlinedocs/gcc/Code-Gen-Options.html#index-fvisibility
embed/frontend_%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c $< -o $@

$(COMPILER_STATIC_LIB): $(COMPILER_API_OBJ)
	ar rcs $@ $^

$(COMPILER_SHARED_LIB): $(COMPILER_API_OBJ) $(EMBED_SHARED_LIB) embed/compiler.exports
	$(CC) -shared -Wl,--version-script=embed/compiler.exports -o $@ $(COMPILER_API_OBJ) -L. -lmonad-embed -lpthread -lgmp -lclang $(shell llvm-config --ldflags --libs $(LLVM_COMPONENTS) --system-libs)

# Compiler binary: statically absorbs runtime, no .so dependency at runtime
$(TARGET): $(OBJS) $(RUNTIME_LIB)
	$(CC) $(CFLAGS) $(EXPORT_LDFLAG) -o $@ $(OBJS) $(RUNTIME_LIB) $(LDFLAGS)

ffi.o: ffi.c $(HEADERS)
	$(CC) $(CFLAGS) $(FFI_CFLAGS) -c $< -o $@

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(RUNTIME_OBJ) $(EMBED_OBJ) $(COMPILER_API_OBJ) $(RUNTIME_LIB) $(EMBED_STATIC_LIB) $(EMBED_SHARED_LIB) $(COMPILER_STATIC_LIB) $(COMPILER_SHARED_LIB) $(TARGET_BASE) $(TARGET_BASE).exe

# Install: monad binary + static archive + core (for linking compiled .mon programs)
install: $(RUNTIME_LIB) $(EMBED_STATIC_LIB) $(EMBED_SHARED_LIB) $(COMPILER_STATIC_LIB) $(COMPILER_SHARED_LIB) $(TARGET)
	install -d $(BINDIR)
	install -m 755 $(TARGET) $(BINDIR)/$(TARGET)
	install -d $(LIBDIR)
	install -m 644 $(RUNTIME_LIB) $(LIBDIR)/$(RUNTIME_LIB)
	install -m 644 $(EMBED_STATIC_LIB) $(LIBDIR)/$(EMBED_STATIC_LIB)
	install -m 755 $(EMBED_SHARED_LIB) $(LIBDIR)/$(EMBED_SHARED_LIB)
	install -m 644 $(COMPILER_STATIC_LIB) $(LIBDIR)/$(COMPILER_STATIC_LIB)
	install -m 755 $(COMPILER_SHARED_LIB) $(LIBDIR)/$(COMPILER_SHARED_LIB)
	install -d $(INCDIR)
	install -m 644 runtime.h $(INCDIR)/runtime.h
	install -m 644 embed/include/monad/embed.h $(INCDIR)/embed.h
	install -m 644 embed/include/monad/monad.h $(INCDIR)/monad.h
	install -m 644 embed/include/monad/qtt.h $(INCDIR)/qtt.h
	install -m 644 embed/include/monad/compiler.h $(INCDIR)/compiler.h
# Install core modules
	rm -rf $(COREDIR)
	find core \( -name "*.mon" -o -name "*.modules" \) | while read f; do \
		rel=$${f#core/}; \
		dir=$$(dirname "$$rel"); \
		install -d $(COREDIR)/$$dir; \
		install -m 644 "$$f" $(COREDIR)/$$dir/; \
	done

uninstall:
	rm -f $(BINDIR)/$(TARGET)
	rm -f $(LIBDIR)/$(RUNTIME_LIB)
	rm -f $(LIBDIR)/$(EMBED_STATIC_LIB)
	rm -f $(LIBDIR)/$(EMBED_SHARED_LIB)
	rm -f $(LIBDIR)/$(COMPILER_STATIC_LIB)
	rm -f $(LIBDIR)/$(COMPILER_SHARED_LIB)
	rm -rf $(INCDIR)
	rm -rf $(PREFIX)/lib/monad/core

ifneq ($(filter core bytecode,$(MAKECMDGOALS)),)
test: all test-embedding
else
test: all test-embedding
	$(PYTHON) tests/run.py
endif

test-embedding: $(EMBED_STATIC_LIB) $(EMBED_SHARED_LIB) $(COMPILER_STATIC_LIB) $(COMPILER_SHARED_LIB)
	$(PYTHON) -m unittest tests.test_embedding tests.test_qtt_foreign_call tests.test_qtt_foreign_lowering tests.test_qtt_foreign_llvm tests.test_embedding_foreign_execution tests.test_embedding_source_unit tests.test_embedding_diagnostics tests.test_embedding_parser_context tests.test_embedding_parser_unwind tests.test_embedding_frontend_capsule tests.test_embedding_ast_transaction tests.test_embedding_frontend_transaction tests.test_embedding_public_parse_diagnostics tests.test_embedding_frontend_parallel tests.test_embedding_frontend_session_state tests.test_embedding_frontend_registry_state tests.test_embedding_frontend_type_state tests.test_embedding_compilation_unit tests.test_embedding_type_diagnostics tests.test_embedding_inference_transaction tests.test_embedding_environment_snapshot tests.test_embedding_environment_queries tests.test_embedding_surface_source tests.test_embedding_source_hooks tests.test_embedding_native_string tests.test_embedding_atomic_redefinition tests.test_embedding_read_generation tests.test_embedding_orc_reclamation tests.test_embedding_releasable_function tests.test_embedding_qtt_report tests.test_embedding_qtt_quantities tests.test_embedding_resource_certificate tests.test_embedding_how_to tests.test_embedding_codegen_convergence

core: test
	$(PYTHON) tests/run_core.py

test-core: all
	$(PYTHON) tests/run_core.py

bytecode: test
	BYTECODE_VISUAL=1 $(PYTHON) tests/test_bytecode.py

test-bytecode: all
	BYTECODE_VISUAL=1 $(PYTHON) tests/test_bytecode.py

generate-asm-tests:
	$(PYTHON) create_asm_tests.py

generate-asm-tests-extra:
	$(PYTHON) create_asm_tests_extra.py

test-runner: all
	MONAD_BINARY=$(CURDIR)/$(TARGET) ./$(TARGET) test runner

test-how-to: all
	MONAD_BINARY=$(CURDIR)/$(TARGET) ./$(TARGET) test how-to

test-context-visualizer:
	$(PYTHON) tests/test_context_visualizer.py

test-context-lint:
	$(PYTHON) tests/test_context_lint.py

test-context-refs:
	$(PYTHON) tests/test_context_refs.py

test-context-graph:
	$(PYTHON) tests/test_context_graph.py

verify-context:
	$(PYTHON) tests/test_context_lint.py --verbose
	$(PYTHON) tests/test_context_refs.py
	$(PYTHON) tests/test_context_visualizer.py
	$(PYTHON) tests/test_context_graph.py
	$(PYTHON) context/tools/context_lint.py --skip-info --check-src-refs --check-test-contexts --check-record-refs

verify-context-strict:
	$(PYTHON) context/tools/context_lint.py --skip-info --all --check-src-refs --check-test-contexts --check-orphaned --check-empty-headings --check-description; \
	echo "---"; \
	echo "NOTE: empty headings, missing descriptions, and orphans are quality metrics, not gate failures"

test-fuzzing: all
	$(PYTHON) tests/fuzzing/fuzz_codegen.py

fuzzing: test-fuzzing

context-visualizer:
	$(PYTHON) context-visualizer.py

install-git-hooks:
	git config core.hooksPath .githooks

verify-push:
	git diff --check
	$(PYTHON) tests/test_gitignore_policy.py
	$(MAKE) test
	$(MAKE) test-core

.PHONY: all clean release install uninstall asan test test-embedding core test-core bytecode test-bytecode generate-asm-tests generate-asm-tests-extra test-runner test-how-to test-context-visualizer test-context-lint test-context-refs test-context-graph verify-context verify-context-strict test-fuzzing fuzzing context-visualizer install-git-hooks verify-push
