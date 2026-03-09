CC      = gcc
CFLAGS  = -Wall -Wextra -std=c99 $(shell llvm-config --cflags)
LDFLAGS = -lm -lreadline $(shell llvm-config --ldflags --libs core)
TARGET  = monad
PREFIX  = /usr/local
BINDIR  = $(PREFIX)/bin
LIBDIR  = $(PREFIX)/lib
INCDIR  = $(PREFIX)/include/monad

DEBUG_CFLAGS   = -g -DDEBUG
RELEASE_CFLAGS = -DNDEBUG -O2

# All .c files EXCEPT runtime.c (built separately as a static archive)
SRCS = $(filter-out runtime.c, $(wildcard *.c))
OBJS = $(SRCS:.c=.o)

# Static archive — no rpath/ldconfig needed, works from any directory
RUNTIME_LIB = libmonad.a
RUNTIME_SRC = runtime.c
RUNTIME_OBJ = runtime.o

all: CFLAGS += $(DEBUG_CFLAGS)
all: $(RUNTIME_LIB) $(TARGET)

release: CFLAGS += $(RELEASE_CFLAGS)
release: $(RUNTIME_LIB) $(TARGET)

$(RUNTIME_OBJ): $(RUNTIME_SRC)
	$(CC) $(CFLAGS) -c $< -o $@

$(RUNTIME_LIB): $(RUNTIME_OBJ)
	ar rcs $@ $^

# Compiler binary: statically absorbs runtime, no .so dependency at runtime
$(TARGET): $(OBJS) $(RUNTIME_LIB)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(RUNTIME_LIB) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(RUNTIME_OBJ) $(RUNTIME_LIB) $(TARGET)

# Install: monad binary + static archive (for linking compiled .mon programs)
install: $(RUNTIME_LIB) $(TARGET)
	install -d $(BINDIR)
	install -m 755 $(TARGET) $(BINDIR)/$(TARGET)
	install -d $(LIBDIR)
	install -m 644 $(RUNTIME_LIB) $(LIBDIR)/$(RUNTIME_LIB)
	install -d $(INCDIR)
	install -m 644 runtime.h $(INCDIR)/runtime.h

uninstall:
	rm -f $(BINDIR)/$(TARGET)
	rm -f $(LIBDIR)/$(RUNTIME_LIB)
	rm -rf $(INCDIR)

.PHONY: all clean release install uninstall
