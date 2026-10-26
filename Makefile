CC = gcc
CFLAGS = -Wall -Wextra -g

# protocol.o has no binary of its own yet; it is listed so make still compiles
# it and catches any breakage before the client and the server arrive
all: myshell protocol.o

myshell: myshell.o parser.o executor.o
	$(CC) $(CFLAGS) -o myshell myshell.o parser.o executor.o

myshell.o: myshell.c myshell.h
	$(CC) $(CFLAGS) -c myshell.c

parser.o: parser.c myshell.h
	$(CC) $(CFLAGS) -c parser.c

executor.o: executor.c myshell.h
	$(CC) $(CFLAGS) -c executor.c

protocol.o: protocol.c protocol.h
	$(CC) $(CFLAGS) -c protocol.c

test: myshell
	bash tests/run_tests.sh

clean:
	rm -f myshell *.o