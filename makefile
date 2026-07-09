SRC ?= main.c
OUT ?= main
CC=gcc
DEFINES=-DDEBUG -DHALT_ON_CORRUPTION
CFLAGS=-std=gnu11 -Wall -Wextra -g
LDFLAGS=-pthread

prod: $(SRC) fmalloc.c
	$(CC) $(DEFINES) $(CFLAGS) $(SRC) fmalloc.c -o ./dist/$(OUT) $(LDFLAGS)
