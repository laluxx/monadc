CC      = gcc
PYTHON  = python3
CFLAGS  = -Wall -Wextra -std=c99 $(shell llvm-config --cflags)
LDFLAGS = -lm -lreadline -lgmp $(shell llvm-config --ldflags --libs core) -lclang
TARGET  = monad
PREFIX  = /usr/local
BINDIR  = $(PREFIX)/bin
LIBDIR  = $(PREFIX)/lib
INCDIR  = $(PREFIX)/include/monad
COREDIR = $(PREFIX)/lib/monad/core

DEBUG_CFLAGS   = -g -DDEBUG
ASAN_CFLAGS    = -g -fsanitize=address -fno-omit-frame-pointer -DDEBUG
RELEASE_CFLAGS = -DNDEBUG -O2

NPROCS = $(shell nproc)
MAKEFLAGS += -j$(NPROCS)

# Static archive — no rpath/ldconfig needed, works from any directory
RUNTIME_LIB = libmonad.a
RUNTIME_SRC = runtime.c arena.c
RUNTIME_OBJ = $(RUNTIME_SRC:.c=.o)

# All .c files EXCEPT runtime sources to prevent concurrent write race conditions in make -j
SRCS = $(filter-out $(RUNTIME_SRC), $(wildcard *.c))
FFI_CFLAGS = $(shell pkg-config --cflags libclang 2>/dev/null || echo "-I/usr/lib/llvm/include")
OBJS = $(SRCS:.c=.o)

all: CFLAGS += $(DEBUG_CFLAGS)
all: $(RUNTIME_LIB) $(TARGET)

asan: CFLAGS += $(ASAN_CFLAGS)
asan: LDFLAGS += -fsanitize=address
asan: $(RUNTIME_LIB) $(TARGET)

release: CFLAGS += $(RELEASE_CFLAGS)
release: $(RUNTIME_LIB) $(TARGET)

$(RUNTIME_OBJ): %.o: %.c
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(RUNTIME_LIB): $(RUNTIME_OBJ)
	ar rcs $@ $^

# Compiler binary: statically absorbs runtime, no .so dependency at runtime
$(TARGET): $(OBJS) $(RUNTIME_LIB)
	$(CC) $(CFLAGS) -rdynamic -o $@ $(OBJS) $(RUNTIME_LIB) $(LDFLAGS)

ffi.o: ffi.c
	$(CC) $(CFLAGS) $(FFI_CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(RUNTIME_OBJ) $(RUNTIME_LIB) $(TARGET)

# Install: monad binary + static archive + core (for linking compiled .mon programs)
install: $(RUNTIME_LIB) $(TARGET)
	install -d $(BINDIR)
	install -m 755 $(TARGET) $(BINDIR)/$(TARGET)
	install -d $(LIBDIR)
	install -m 644 $(RUNTIME_LIB) $(LIBDIR)/$(RUNTIME_LIB)
	install -d $(INCDIR)
	install -m 644 runtime.h $(INCDIR)/runtime.h
# Install core modules
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

ifneq ($(filter core,$(MAKECMDGOALS)),)
test: all
else
test: all
	$(PYTHON) tests/run.py
endif

core: test
	$(PYTHON) tests/run_core.py

test-core: all
	$(PYTHON) tests/run_core.py

generate-asm-tests:
	$(PYTHON) create_asm_tests.py

generate-asm-tests-extra:
	$(PYTHON) create_asm_tests_extra.py

test-runner:
	$(PYTHON) tests/test_run.py

test-context-visualizer:
	$(PYTHON) tests/test_context_visualizer.py

test-fuzzing: all
	$(PYTHON) tests/fuzzing/fuzz_codegen.py

fuzzing: test-fuzzing

context-visualizer:
	$(PYTHON) context-visualizer.py

.PHONY: all clean release install uninstall asan test core test-core generate-asm-tests generate-asm-tests-extra test-runner test-context-visualizer test-fuzzing fuzzing context-visualizer
