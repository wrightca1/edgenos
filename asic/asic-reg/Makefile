CROSS_COMPILE ?=
CC      = $(CROSS_COMPILE)gcc
CFLAGS  = -Wall -Wextra -O2
LDFLAGS = -static
TARGET  = asic-reg

all: $(TARGET)

$(TARGET): asic-reg.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

.PHONY: all clean
