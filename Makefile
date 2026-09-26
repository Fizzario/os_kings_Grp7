CC = gcc
CFLAGS = -Wall -Wextra -g

myshell: myshell.o parser.o
	$(CC) $(CFLAGS) -o myshell myshell.o parser.o

myshell.o: myshell.c myshell.h
	$(CC) $(CFLAGS) -c myshell.c

parser.o: parser.c myshell.h
	$(CC) $(CFLAGS) -c parser.c

clean:
	rm -f myshell *.o