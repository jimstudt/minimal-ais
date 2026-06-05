CC ?= cc
CFLAGS ?= -Wall -Wextra -pedantic -std=c99 -O2
LDFLAGS ?=

TARGET = minimal-ais
SRCS = main.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

clean:
	rm -f $(TARGET) $(OBJS)
