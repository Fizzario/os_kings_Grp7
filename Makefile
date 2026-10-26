CC = gcc
CFLAGS = -Wall -Wextra -g

# myshell is the local shell from phase 1 and still runs on its own
# myclient is the remote front end; myserver is added with the server
all: myshell myclient

myshell: myshell.o parser.o executor.o
	$(CC) $(CFLAGS) -o myshell myshell.o parser.o executor.o

myshell.o: myshell.c myshell.h
	$(CC) $(CFLAGS) -c myshell.c

parser.o: parser.c myshell.h
	$(CC) $(CFLAGS) -c parser.c

executor.o: executor.c myshell.h
	$(CC) $(CFLAGS) -c executor.c

# the client only moves text, so it needs the framing and the shared limits
# but none of the parsing or execution code
myclient: client.o protocol.o
	$(CC) $(CFLAGS) -o myclient client.o protocol.o

client.o: client.c myshell.h protocol.h
	$(CC) $(CFLAGS) -c client.c

protocol.o: protocol.c protocol.h
	$(CC) $(CFLAGS) -c protocol.c

# the shell's own parser cases, from phase 1
test: myshell
	bash tests/run_tests.sh

# the framing on its own: partial reads, large payloads, a peer that dies
test-protocol: tests/test_protocol
	./tests/test_protocol

# the client against the stand in server in tests/
test-client: myclient tests/testserver
	bash tests/run_client_tests.sh

test-all: test test-protocol test-client

tests/test_protocol: tests/test_protocol.c protocol.c protocol.h
	$(CC) $(CFLAGS) -I. -o tests/test_protocol tests/test_protocol.c protocol.c

# a stand in server, used only by the client tests; not the project's server
tests/testserver: tests/testserver.c protocol.c protocol.h
	$(CC) $(CFLAGS) -I. -o tests/testserver tests/testserver.c protocol.c

clean:
	rm -f myshell myclient *.o tests/test_protocol tests/testserver