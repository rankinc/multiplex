SRC=multiplex.c
OBJ=$(SRC:.c=.o)

CFLAGS=-Wall -W -std=c11 -O2 -g -I/usr/include/libevdev-1.0 -D_POSIX_C_SOURCE=200809L
LDFLAGS=-Wl,-sort-common -Wl,-zcombreloc -Wl,-znoexecstack -Wl,-zrelro -levdev
CC=gcc

.PHONY: all clean depend

all: multiplex

depend: $(SRC)
	@$(CC) -MM $^ > depend.mak

clean:
	$(RM) multiplex $(OBJ)

multiplex: multiplex.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $<

-include depend.mak
