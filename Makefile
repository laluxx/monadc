CC          ?= gcc
PYTHON      ?= python3
AR          ?= ar
TARGET_BASE  = monad
PREFIX      ?= /usr/local
BINDIR       = $(PREFIX)/bin
LIBDIR       = $(PREFIX)/lib
INCDIR       = $(PREFIX)/include/monad
COREDIR      = $(PREFIX)/lib/monad/core
CORE_CACHE_DIR ?= $(HOME)/.cache/monad/core
PREWARM_REPL_CACHE ?= 1

SRC_DIR      = src
BUILD_DIR    = build
OBJ_DIR      = $(BUILD_DIR)/obj
BIN_OUT_DIR  = $(BUILD_DIR)/bin
LIB_OUT_DIR  = $(BUILD_DIR)/lib

UNAME_S      := $(shell uname -s 2>/dev/null || echo unknown)
WINDOWS_HOST := $(if $(filter MINGW% MSYS% CYGWIN%,$(UNAME_S)),1,)
EXEEXT       := $(if $(WINDOWS_HOST),.exe,)
TARGET       = $(TARGET_BASE)$(EXEEXT)
TARGET_PATH  = $(BIN_OUT_DIR)/$(TARGET)
EXPORT_LDFLAG := $(if $(WINDOWS_HOST),,-rdynamic)

LLVM_COMPONENTS = core orcjit native passes
LLVM_CFLAGS  := $(shell llvm-config --cflags 2>/dev/null)
LLVM_LDFLAGS := $(shell llvm-config --ldflags --libs $(LLVM_COMPONENTS) 2>/dev/null)
FFI_CFLAGS   := $(shell pkg-config --cflags libclang 2>/dev/null || echo "-I/usr/lib/llvm/include")

CPPFLAGS += -iquote$(SRC_DIR) -iquote$(SRC_DIR)/tooling -I$(SRC_DIR)/embed/include
CFLAGS   += -Wall -Wextra -std=c99 -fPIC $(LLVM_CFLAGS)
LDFLAGS  += -lm -lreadline -lpthread -lgmp $(LLVM_LDFLAGS) -lclang

DEBUG_CFLAGS   = -g -DDEBUG
ASAN_CFLAGS    = -g -fsanitize=address -fno-omit-frame-pointer -DDEBUG
UBSAN_CFLAGS   = -g -fsanitize=undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -DDEBUG
RELEASE_CFLAGS = -DNDEBUG -O2
PERF_CFLAGS    = -O2 -g -fno-omit-frame-pointer -DNDEBUG

RUNTIME_LIB          = libmonad.a
EMBED_STATIC_LIB     = libmonad-embed.a
EMBED_SHARED_LIB     = libmonad-embed.so
COMPILER_STATIC_LIB  = libmonad-compiler.a
COMPILER_SHARED_LIB  = libmonad-compiler.so

RUNTIME_LIB_PATH         = $(LIB_OUT_DIR)/$(RUNTIME_LIB)
EMBED_STATIC_LIB_PATH    = $(LIB_OUT_DIR)/$(EMBED_STATIC_LIB)
EMBED_SHARED_LIB_PATH    = $(LIB_OUT_DIR)/$(EMBED_SHARED_LIB)
COMPILER_STATIC_LIB_PATH = $(LIB_OUT_DIR)/$(COMPILER_STATIC_LIB)
COMPILER_SHARED_LIB_PATH = $(LIB_OUT_DIR)/$(COMPILER_SHARED_LIB)

RUNTIME_SRC = \
  $(SRC_DIR)/arena.c \
  $(SRC_DIR)/runtime.c \
  $(SRC_DIR)/runtime_errors.c
RUNTIME_OBJ = $(patsubst %.c,$(OBJ_DIR)/runtime/%.o,$(RUNTIME_SRC))

ROOT_COMPILER_SRC = $(filter-out $(RUNTIME_SRC),$(wildcard $(SRC_DIR)/*.c))
SUBSYSTEM_SRC = \
  $(wildcard $(SRC_DIR)/qtt/*.c) \
  $(wildcard $(SRC_DIR)/concurrency/*.c) \
  $(wildcard $(SRC_DIR)/effects/*.c) \
  $(wildcard $(SRC_DIR)/tooling/*.c)
ifeq ($(WINDOWS_HOST),1)
ROOT_COMPILER_SRC := $(filter-out $(SRC_DIR)/debugger.c,$(ROOT_COMPILER_SRC))
endif
COMPILER_SRC = $(ROOT_COMPILER_SRC) $(SUBSYSTEM_SRC)
COMPILER_OBJ = $(patsubst %.c,$(OBJ_DIR)/compiler/%.o,$(COMPILER_SRC))

HEADERS = \
  $(wildcard $(SRC_DIR)/*.h) \
  $(wildcard $(SRC_DIR)/qtt/*.h) \
  $(wildcard $(SRC_DIR)/concurrency/*.h) \
  $(wildcard $(SRC_DIR)/effects/*.h) \
  $(wildcard $(SRC_DIR)/tooling/*.h) \
  $(wildcard $(SRC_DIR)/embed/*.h) \
  $(wildcard $(SRC_DIR)/embed/include/monad/*.h)

EMBED_SOURCES = $(SRC_DIR)/embed/embed.c $(SRC_DIR)/embed/monad.c
EMBED_OBJ = $(patsubst %.c,$(OBJ_DIR)/embed/%.o,$(EMBED_SOURCES))

COMPILER_API_SOURCES = \
  $(SRC_DIR)/embed/compiler.c \
  $(SRC_DIR)/embed/surface_compiler.c \
  $(SRC_DIR)/embed/surface_load.c \
  $(SRC_DIR)/embed/compiler_native.c \
  $(SRC_DIR)/embed/frontend_transaction.c \
  $(SRC_DIR)/embed/native_compile.c \
  $(SRC_DIR)/embed/infer_support.c \
  $(SRC_DIR)/infer.c \
  $(SRC_DIR)/features.c \
  $(SRC_DIR)/macro.c \
  $(SRC_DIR)/pmatch.c \
  $(SRC_DIR)/reader.c \
  $(SRC_DIR)/reader_diagnostic.c \
  $(SRC_DIR)/reader_syntax.c \
  $(SRC_DIR)/types.c \
  $(SRC_DIR)/wisp.c \
  $(SRC_DIR)/wisp_syntax_policy.c \
  $(SRC_DIR)/codegen.c \
  $(SRC_DIR)/env.c \
  $(SRC_DIR)/typeclass.c \
  $(SRC_DIR)/module.c \
  $(SRC_DIR)/asm.c \
  $(SRC_DIR)/ffi.c \
  $(SRC_DIR)/effects/effect.c \
  $(SRC_DIR)/effects/constraints.c \
  $(SRC_DIR)/qtt/foreign_type.c \
  $(SRC_DIR)/qtt/type_identity.c \
  $(SRC_DIR)/qtt/constraints.c \
  $(SRC_DIR)/qtt/environment.c \
  $(SRC_DIR)/qtt/quantity.c \
  $(SRC_DIR)/qtt/bindings.c \
  $(SRC_DIR)/qtt/elaboration.c \
  $(SRC_DIR)/qtt/pipeline.c \
  $(SRC_DIR)/qtt/anf.c \
  $(SRC_DIR)/qtt/core.c \
  $(SRC_DIR)/qtt/demand.c \
  $(SRC_DIR)/qtt/graded.c \
  $(SRC_DIR)/qtt/signature.c \
  $(SRC_DIR)/qtt/call.c \
  $(SRC_DIR)/qtt/resource.c \
  $(SRC_DIR)/qtt/signature_env.c \
  $(SRC_DIR)/qtt/backend.c \
  $(SRC_DIR)/qtt/compiler.c \
  $(SRC_DIR)/qtt/compiler_module.c \
  $(SRC_DIR)/qtt/core_effect.c \
  $(SRC_DIR)/qtt/interface.c \
  $(SRC_DIR)/qtt/semantic_ir.c \
  $(SRC_DIR)/qtt/effect_runtime.c \
  $(SRC_DIR)/qtt/module.c \
  $(SRC_DIR)/qtt/drop.c \
  $(SRC_DIR)/qtt/evidence.c \
  $(SRC_DIR)/qtt/closure_policy.c \
  $(SRC_DIR)/qtt/closure.c \
  $(SRC_DIR)/qtt/core_usage.c \
  $(SRC_DIR)/qtt/semantic_anf.c
COMPILER_API_OBJ = $(patsubst %.c,$(OBJ_DIR)/compiler-api/%.o,$(COMPILER_API_SOURCES)) $(RUNTIME_OBJ)

.PHONY: all debug release asan ubsan perf clean install uninstall test test-core core \
        test-embedding repl bytecode test-bytecode generate-asm-tests generate-asm-tests-extra \
        test-runner test-how-to test-context-visualizer test-context-lint test-context-refs \
        test-context-graph verify-context verify-context-strict test-fuzzing fuzzing \
        context-visualizer install-git-hooks verify-push

all: CFLAGS += $(DEBUG_CFLAGS)
all: $(RUNTIME_LIB_PATH) $(EMBED_STATIC_LIB_PATH) $(EMBED_SHARED_LIB_PATH) \
     $(COMPILER_STATIC_LIB_PATH) $(COMPILER_SHARED_LIB_PATH) $(TARGET_PATH)

debug: all

release: CFLAGS += $(RELEASE_CFLAGS)
release: $(RUNTIME_LIB_PATH) $(EMBED_STATIC_LIB_PATH) $(EMBED_SHARED_LIB_PATH) \
         $(COMPILER_STATIC_LIB_PATH) $(COMPILER_SHARED_LIB_PATH) $(TARGET_PATH)

asan: CFLAGS += $(ASAN_CFLAGS)
asan: LDFLAGS += -fsanitize=address
asan: $(RUNTIME_LIB_PATH) $(EMBED_STATIC_LIB_PATH) $(EMBED_SHARED_LIB_PATH) \
      $(COMPILER_STATIC_LIB_PATH) $(COMPILER_SHARED_LIB_PATH) $(TARGET_PATH)

ubsan: CFLAGS += $(UBSAN_CFLAGS)
ubsan: LDFLAGS += -fsanitize=undefined
ubsan: $(RUNTIME_LIB_PATH) $(EMBED_STATIC_LIB_PATH) $(EMBED_SHARED_LIB_PATH) \
       $(COMPILER_STATIC_LIB_PATH) $(COMPILER_SHARED_LIB_PATH) $(TARGET_PATH)

perf: CFLAGS += $(PERF_CFLAGS)
perf: $(RUNTIME_LIB_PATH) $(EMBED_STATIC_LIB_PATH) $(EMBED_SHARED_LIB_PATH) \
      $(COMPILER_STATIC_LIB_PATH) $(COMPILER_SHARED_LIB_PATH) $(TARGET_PATH)

$(OBJ_DIR)/runtime/%.o: %.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/compiler/%.o: %.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(if $(filter $(SRC_DIR)/ffi.c,$<),$(FFI_CFLAGS),) -c $< -o $@

$(OBJ_DIR)/embed/%.o: %.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -Wall -Wextra -std=c99 -fPIC -fvisibility=hidden -DMONAD_EMBED_BUILD -c $< -o $@

$(OBJ_DIR)/compiler-api/%.o: %.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(if $(filter $(SRC_DIR)/ffi.c,$<),$(FFI_CFLAGS),) \
	      -fvisibility=hidden -DMONAD_COMPILER_BUILD -c $< -o $@

$(RUNTIME_LIB_PATH): $(RUNTIME_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(EMBED_STATIC_LIB_PATH): $(EMBED_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(EMBED_SHARED_LIB_PATH): $(EMBED_OBJ)
	@mkdir -p $(dir $@)
	$(CC) -shared -o $@ $^ -lpthread

$(COMPILER_STATIC_LIB_PATH): $(COMPILER_API_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(COMPILER_SHARED_LIB_PATH): $(COMPILER_API_OBJ) $(EMBED_SHARED_LIB_PATH) $(SRC_DIR)/embed/compiler.exports
	@mkdir -p $(dir $@)
	$(CC) -shared -Wl,--version-script=$(SRC_DIR)/embed/compiler.exports -o $@ $(COMPILER_API_OBJ) \
	      -L$(LIB_OUT_DIR) -lmonad-embed -lpthread -lgmp -lclang $(LLVM_LDFLAGS)

$(TARGET_PATH): $(COMPILER_OBJ) $(RUNTIME_LIB_PATH)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(EXPORT_LDFLAG) -o $@ $(COMPILER_OBJ) $(RUNTIME_LIB_PATH) $(LDFLAGS)

clean:
	rm -rf $(OBJ_DIR) $(BIN_OUT_DIR) $(LIB_OUT_DIR)

install: all
	install -d $(BINDIR) $(LIBDIR) $(INCDIR)
	install -m 755 $(TARGET_PATH) $(BINDIR)/$(TARGET)
	install -m 644 $(RUNTIME_LIB_PATH) $(LIBDIR)/$(RUNTIME_LIB)
	install -m 644 $(EMBED_STATIC_LIB_PATH) $(LIBDIR)/$(EMBED_STATIC_LIB)
	install -m 755 $(EMBED_SHARED_LIB_PATH) $(LIBDIR)/$(EMBED_SHARED_LIB)
	install -m 644 $(COMPILER_STATIC_LIB_PATH) $(LIBDIR)/$(COMPILER_STATIC_LIB)
	install -m 755 $(COMPILER_SHARED_LIB_PATH) $(LIBDIR)/$(COMPILER_SHARED_LIB)
	install -m 644 $(SRC_DIR)/runtime.h $(INCDIR)/runtime.h
	install -m 644 $(SRC_DIR)/embed/include/monad/embed.h $(INCDIR)/embed.h
	install -m 644 $(SRC_DIR)/embed/include/monad/monad.h $(INCDIR)/monad.h
	install -m 644 $(SRC_DIR)/embed/include/monad/qtt.h $(INCDIR)/qtt.h
	install -m 644 $(SRC_DIR)/embed/include/monad/compiler.h $(INCDIR)/compiler.h
	rm -rf $(COREDIR)
	find core -name '*.mon' ! -name '.#*' | while read f; do \
		rel=$${f#core/}; dir=$$(dirname "$$rel"); \
		install -d "$(COREDIR)/$$dir"; install -m 644 "$$f" "$(COREDIR)/$$dir/"; \
	done
	rm -rf "$(CORE_CACHE_DIR)"
	install -d "$(CORE_CACHE_DIR)"
	@if [ "$(PREWARM_REPL_CACHE)" = "1" ]; then "$(BINDIR)/$(TARGET)" </dev/null >/dev/null; fi

uninstall:
	rm -f $(BINDIR)/$(TARGET)
	rm -f $(LIBDIR)/$(RUNTIME_LIB) $(LIBDIR)/$(EMBED_STATIC_LIB) $(LIBDIR)/$(EMBED_SHARED_LIB)
	rm -f $(LIBDIR)/$(COMPILER_STATIC_LIB) $(LIBDIR)/$(COMPILER_SHARED_LIB)
	rm -rf $(INCDIR) $(COREDIR)

test: all
	MONAD_BINARY=$(CURDIR)/$(TARGET_PATH) $(PYTHON) -B -m src.testing.runner

test-core core: all
	MONAD_BINARY=$(CURDIR)/$(TARGET_PATH) $(PYTHON) -B -m src.testing.core_runner

test-embedding: all
	MONAD_BINARY=$(CURDIR)/$(TARGET_PATH) $(PYTHON) -B -m unittest discover -s src/testing/contracts -p 'test_embedding*.py'

# Focused REPL contract suite. The REPL implementation itself lives in src/tooling/.
repl: all
	MONAD_BINARY=$(CURDIR)/$(TARGET_PATH) $(PYTHON) -B -m unittest \
		src.testing.contracts.test_repl src.testing.contracts.test_repl_pty src.testing.contracts.test_repl_cache

bytecode test-bytecode: all
	BYTECODE_VISUAL=1 MONAD_BINARY=$(CURDIR)/$(TARGET_PATH) $(PYTHON) -B -m src.testing.contracts.test_bytecode

generate-asm-tests:
	$(PYTHON) -B src/tooling/generators/create_asm_tests.py

generate-asm-tests-extra:
	$(PYTHON) -B src/tooling/generators/create_asm_tests_extra.py

test-runner: all
	MONAD_BINARY=$(CURDIR)/$(TARGET_PATH) $(TARGET_PATH) test runner

test-how-to: all
	MONAD_BINARY=$(CURDIR)/$(TARGET_PATH) $(PYTHON) -B -m src.testing.contracts.test_how_to_examples

test-context-visualizer:
	$(PYTHON) -B -m src.testing.contracts.test_context_visualizer

test-context-lint:
	$(PYTHON) -B -m src.testing.contracts.test_context_lint

test-context-refs:
	$(PYTHON) -B -m src.testing.contracts.test_context_refs

test-context-graph:
	$(PYTHON) -B -m src.testing.contracts.test_context_graph

verify-context:
	$(PYTHON) -B -m src.testing.contracts.test_context_lint --verbose
	$(PYTHON) -B -m src.testing.contracts.test_context_refs
	$(PYTHON) -B -m src.testing.contracts.test_context_visualizer
	$(PYTHON) -B -m src.testing.contracts.test_context_graph
	$(PYTHON) -B src/tooling/context/context_lint.py --skip-info --check-src-refs --check-test-contexts --check-record-refs

verify-context-strict:
	$(PYTHON) -B src/tooling/context/context_lint.py --skip-info --all --check-src-refs --check-test-contexts --check-orphaned --check-empty-headings --check-description; \
	echo '---'; echo 'NOTE: empty headings, missing descriptions, and orphans are quality metrics, not gate failures'

test-fuzzing fuzzing: all
	MONAD_BINARY=$(CURDIR)/$(TARGET_PATH) $(PYTHON) -B -m src.testing.contracts.fuzzing.fuzz_codegen

context-visualizer:
	$(PYTHON) -B src/tooling/context/visualizer.py

install-git-hooks:
	./make hooks

verify-push:
	./make clean --check
	./make check
