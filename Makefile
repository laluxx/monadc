CC = gcc
CFLAGS = -Wall -Wextra -std=c99 $(shell llvm-config --cflags)
LDFLAGS = -lm -lreadline $(shell llvm-config --ldflags --libs core)
TARGET = monad

DEBUG_CFLAGS = -g -DDEBUG
RELEASE_CFLAGS = -DNDEBUG -O2

SRCS = $(wildcard *.c)
OBJS = $(SRCS:.c=.o)

all: CFLAGS += $(DEBUG_CFLAGS)
all: $(TARGET)

release: CFLAGS += $(RELEASE_CFLAGS)
release: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

run: $(TARGET)
	./$(TARGET) src.mon

.PHONY: all clean run release
