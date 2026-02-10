CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -g $(shell llvm-config --cflags)
LDFLAGS = -lm $(shell llvm-config --ldflags --libs core)
TARGET = mc

SRCS = $(wildcard *.c)
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

run: $(TARGET)
	./$(TARGET) src.mon

.PHONY: all clean run
