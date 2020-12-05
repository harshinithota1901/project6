CC=gcc
CFLAGS=-Wall -ggdb

default: master user

shared.o: shared.c master.h
	$(CC) $(CFLAGS) -c shared.c

blockedq.o: blockedq.c blockedq.h
	$(CC) $(CFLAGS) -c blockedq.c

memory.o: memory.c memory.h
	$(CC) $(CFLAGS) -c memory.c

master: master.c master.h memory.o blockedq.o
	$(CC) $(CFLAGS) master.c memory.o blockedq.o -o master

user: user.c master.h
	$(CC) $(CFLAGS) user.c -o user -lm

clean:
	rm -f master user *.o
