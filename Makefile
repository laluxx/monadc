CC      = gcc
CFLAGS  = -Wall -Wextra -std=c99 $(shell llvm-config --cflags)
LDFLAGS = -lm -lreadline -lgmp $(shell llvm-config --ldflags --libs core) -lclang
TARGET  = monad
PREFIX  = /usr/local
BINDIR  = $(PREFIX)/bin
LIBDIR  = $(PREFIX)/lib
INCDIR  = $(PREFIX)/include/monad
COREDIR = $(PREFIX)/lib/monad/core

DEBUG_CFLAGS   = -g -DDEBUG
RELEASE_CFLAGS = -DNDEBUG -O2

# All .c files EXCEPT runtime.c (built separately as a static archive)
SRCS = $(filter-out runtime.c, $(wildcard *.c))
FFI_CFLAGS = $(shell pkg-config --cflags libclang 2>/dev/null || echo "-I/usr/lib/llvm/include")
OBJS = $(SRCS:.c=.o)

# Static archive — no rpath/ldconfig needed, works from any directory
RUNTIME_LIB = libmonad.a
RUNTIME_SRC = runtime.c arena.c
RUNTIME_OBJ = runtime.o arena.o

all: CFLAGS += $(DEBUG_CFLAGS)
all: $(RUNTIME_LIB) $(TARGET)

release: CFLAGS += $(RELEASE_CFLAGS)
release: $(RUNTIME_LIB) $(TARGET)


$(RUNTIME_OBJ): %.o: %.c
	$(CC) $(CFLAGS) -fPIC -c $< -o $@
# $(RUNTIME_OBJ): $(RUNTIME_SRC)
# 	$(CC) $(CFLAGS) -fPIC -c $< -o $@

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

.PHONY: all clean release install uninstall
