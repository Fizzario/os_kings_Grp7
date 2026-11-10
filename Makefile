CC = gcc
CFLAGS = -Wall -Wextra -g

# the server serves each client on its own thread, so it and anything it
# links have to be built and linked for threads
PTHREAD = -pthread

# myshell is the local shell from phase 1 and still runs on its own
# myclient and myserver are the two ends of the remote shell
all: myshell myclient myserver

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

# the server runs the same parser and executor as the local shell, and adds
# the per-client log, which needs threads
myserver: server.o parser.o executor.o protocol.o clientlog.o
	$(CC) $(CFLAGS) $(PTHREAD) -o myserver server.o parser.o executor.o protocol.o clientlog.o

server.o: server.c myshell.h protocol.h clientlog.h
	$(CC) $(CFLAGS) $(PTHREAD) -c server.c

# who each client is, and the log of what it does; one mutex keeps an entry
# from being cut in half when several threads log at once
clientlog.o: clientlog.c clientlog.h
	$(CC) $(CFLAGS) $(PTHREAD) -c clientlog.c

# the shell's own parser cases, from phase 1
test: myshell
	bash tests/run_tests.sh

# the framing on its own: partial reads, large payloads, a peer that dies
test-protocol: tests/test_protocol
	./tests/test_protocol

# the client against the stand in server in tests/
test-client: myclient tests/testserver
	bash tests/run_client_tests.sh

# the real client against the real server
test-server: myshell myclient myserver
	bash tests/run_server_tests.sh

# the log on its own, with many threads writing to it at once
test-log: tests/test_clientlog
	./tests/test_clientlog

# several clients against one server at once: the point of phase 3
# checks that need one thread per client report PENDING until that is in
test-concurrent: myserver myclient
	bash tests/run_concurrent_tests.sh

# everything is built first, so a compile error is reported straight away
# rather than after a suite's worth of output has scrolled past
test-all: all test test-protocol test-client test-log test-server test-concurrent

tests/test_protocol: tests/test_protocol.c protocol.c protocol.h
	$(CC) $(CFLAGS) -I. -o tests/test_protocol tests/test_protocol.c protocol.c

tests/test_clientlog: tests/test_clientlog.c clientlog.c clientlog.h
	$(CC) $(CFLAGS) $(PTHREAD) -I. -o tests/test_clientlog tests/test_clientlog.c clientlog.c

# a stand in server, used only by the client tests; not the project's server
tests/testserver: tests/testserver.c protocol.c protocol.h
	$(CC) $(CFLAGS) -I. -o tests/testserver tests/testserver.c protocol.c

clean:
	rm -f myshell myclient myserver *.o \
	      tests/test_protocol tests/testserver tests/test_clientlog