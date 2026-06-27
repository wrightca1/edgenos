# ONLP platform library Makefile
CROSS_COMPILE ?= powerpc-linux-gnu-
CC = $(CROSS_COMPILE)gcc
AR = $(CROSS_COMPILE)ar
CFLAGS = -Wall -Wextra -O2 -g -fPIC

SRCS = platform_lib.c sfpi.c
OBJS = $(SRCS:.c=.o)
LIB = libplatform.a

.PHONY: all clean

all: $(LIB)

$(LIB): $(OBJS)
	$(AR) rcs $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(LIB)
