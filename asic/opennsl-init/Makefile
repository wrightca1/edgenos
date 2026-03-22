CROSS_COMPILE ?= powerpc-linux-gnu-
CC = $(CROSS_COMPILE)gcc
INCLUDES = -IOpenNSL-master/include
CFLAGS = -Wall -O2 $(INCLUDES)
# Use dlopen at runtime instead of linking directly — libopennsl was
# built with switch's glibc 2.19 which doesn't match Docker's glibc.
LDFLAGS = -ldl
TARGET = opennsl-init

all: $(TARGET)

$(TARGET): opennsl-init.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
