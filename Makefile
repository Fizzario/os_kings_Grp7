CC = gcc
CFLAGS = -Wall -Wextra -g

myshell: myshell.o parser.o executor.o
	$(CC) $(CFLAGS) -o myshell myshell.o parser.o executor.o

myshell.o: myshell.c myshell.h
	$(CC) $(CFLAGS) -c myshell.c

parser.o: parser.c myshell.h
	$(CC) $(CFLAGS) -c parser.c

executor.o: executor.c myshell.h
	$(CC) $(CFLAGS) -c executor.c

test: myshell
	bash tests/run_tests.sh

clean:
	rm -f myshell *.o