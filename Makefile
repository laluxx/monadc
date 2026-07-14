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
LLVM_COMPONENTS = core orcjit native
LDFLAGS = -lm -lreadline -lpthread -lgmp $(shell llvm-config --ldflags --libs $(LLVM_COMPONENTS)) -lclang

DEBUG_CFLAGS   = -g -DDEBUG
ASAN_CFLAGS    = -g -fsanitize=address -fno-omit-frame-pointer -DDEBUG
RELEASE_CFLAGS = -DNDEBUG -O2

NPROCS = $(shell nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
MAKEFLAGS += -j$(NPROCS)

# Static archive — no rpath/ldconfig needed, works from any directory
RUNTIME_LIB = libmonad.a
RUNTIME_SRC = runtime.c arena.c
RUNTIME_OBJ = $(RUNTIME_SRC:.c=.o)
HEADERS = $(wildcard *.h)

# All compiler .c files except runtime sources and platform-only sources.
WINDOWS_EXCLUDED_SRCS =
ifeq ($(WINDOWS_HOST),1)
WINDOWS_EXCLUDED_SRCS = debugger.c
endif
COMPILER_EXCLUDED_SRCS = $(RUNTIME_SRC) $(WINDOWS_EXCLUDED_SRCS)
SRCS = $(filter-out $(COMPILER_EXCLUDED_SRCS), $(wildcard *.c))
FFI_CFLAGS = $(shell pkg-config --cflags libclang 2>/dev/null || echo "-I/usr/lib/llvm/include")
OBJS = $(SRCS:.c=.o)

all: CFLAGS += $(DEBUG_CFLAGS)
all: $(RUNTIME_LIB) $(TARGET)

asan: CFLAGS += $(ASAN_CFLAGS)
asan: LDFLAGS += -fsanitize=address
asan: $(RUNTIME_LIB) $(TARGET)

release: CFLAGS += $(RELEASE_CFLAGS)
release: $(RUNTIME_LIB) $(TARGET)

$(RUNTIME_OBJ): %.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(RUNTIME_LIB): $(RUNTIME_OBJ)
	ar rcs $@ $^

# Compiler binary: statically absorbs runtime, no .so dependency at runtime
$(TARGET): $(OBJS) $(RUNTIME_LIB)
	$(CC) $(CFLAGS) $(EXPORT_LDFLAG) -o $@ $(OBJS) $(RUNTIME_LIB) $(LDFLAGS)

ffi.o: ffi.c $(HEADERS)
	$(CC) $(CFLAGS) $(FFI_CFLAGS) -c $< -o $@

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(RUNTIME_OBJ) $(RUNTIME_LIB) $(TARGET_BASE) $(TARGET_BASE).exe

# Install: monad binary + static archive + core (for linking compiled .mon programs)
install: $(RUNTIME_LIB) $(TARGET)
	install -d $(BINDIR)
	install -m 755 $(TARGET) $(BINDIR)/$(TARGET)
	install -d $(LIBDIR)
	install -m 644 $(RUNTIME_LIB) $(LIBDIR)/$(RUNTIME_LIB)
	install -d $(INCDIR)
	install -m 644 runtime.h $(INCDIR)/runtime.h
# Install core modules
	rm -rf $(COREDIR)
	find core -name "*.mon" | while read f; do \
		dir=$$(dirname "$$f"); \
		install -d $(COREDIR)/$${dir#core/}; \
		install -m 644 "$$f" $(COREDIR)/$${dir#core/}/; \
	done

uninstall:
	rm -f $(BINDIR)/$(TARGET)
	rm -f $(LIBDIR)/$(RUNTIME_LIB)
	rm -rf $(INCDIR)
	rm -rf $(PREFIX)/lib/monad/core

ifneq ($(filter core bytecode,$(MAKECMDGOALS)),)
test: all
else
test: all
	$(PYTHON) tests/run.py
endif

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

.PHONY: all clean release install uninstall asan test core test-core bytecode test-bytecode generate-asm-tests generate-asm-tests-extra test-runner test-how-to test-context-visualizer test-context-lint test-context-refs test-context-graph verify-context verify-context-strict test-fuzzing fuzzing context-visualizer
