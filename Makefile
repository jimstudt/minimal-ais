CC ?= cc
CFLAGS ?= -Wall -Wextra -pedantic -std=c99 -O2
LDFLAGS ?=

TARGETS = minimal-ais-serial/minimal-ais-serial minimal-ais-json/minimal-ais-json

SERIAL_SRCS = minimal-ais-serial/main.c minimal-ais-serial/tracking.c
SERIAL_OBJS = $(SERIAL_SRCS:.c=.o)

JSON_SRCS = minimal-ais-json/main.c
JSON_OBJS = $(JSON_SRCS:.c=.o)

.PHONY: all clean

all: $(TARGETS)

minimal-ais-serial/minimal-ais-serial: $(SERIAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(SERIAL_OBJS) $(LDFLAGS)

minimal-ais-json/minimal-ais-json: $(JSON_OBJS)
	$(CC) $(CFLAGS) -o $@ $(JSON_OBJS) $(LDFLAGS)

clean:
	rm -f $(TARGETS) $(SERIAL_OBJS) $(JSON_OBJS)
