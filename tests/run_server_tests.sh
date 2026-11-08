#!/bin/bash
#
# runs the real client against the real server and checks each result
#
# run_client_tests.sh uses a stand in server so it only covers the client,
# this one covers the server: commands really run, so the checks compare
# against what myshell prints for the same line

ROOT=$(pwd)
WORK=$ROOT/tests/scratch
OUT=$ROOT/tests/.client_out
LOG=$ROOT/tests/.server_log
pass=0
fail=0

check() {
  if [ "$2" = "$3" ]; then
    printf '  %-52s PASS\n' "$1"
    pass=$((pass + 1))
  else
    printf '  %-52s FAIL (expected %s, got %s)\n' "$1" "$2" "$3"
    fail=$((fail + 1))
  fi
}

# start the server in a directory and wait until it says it is listening
start_server() {
  : > "$LOG"
  (cd "$1" && exec "$ROOT/myserver") > "$LOG" 2>&1 &
  srv=$!
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    grep -q waiting "$LOG" && break
    sleep 0.1
  done
}

# since phase 3 the server does not stop when a client leaves, so the suite
# has to stop it itself rather than waiting for it to go
# 0 while the server is still running
server_alive() {
  kill -0 "$srv" 2>/dev/null
}

# stops the server and reaps it, so the port is free for the next case
stop_server() {
  kill -TERM "$srv" 2>/dev/null
  wait "$srv" 2>/dev/null
  server_status=$?
}

# 0 if the server stopped on its own within 3 seconds; used only where a
# session is expected NOT to end the server, to prove it stayed up
server_outlives_client() {
  sleep 0.4           # time to log the disconnect
  server_alive
}

# waits up to 5s for no process matching $1 to be left, then reports how
# many there are. a command the server is still reaping has not leaked, it
# simply has not finished, so the count is taken once it has had the chance
remaining() {
  for _ in $(seq 50); do
    [ "$(pgrep -u "$(id -u)" -f "^$1" | wc -l | tr -d ' ')" -eq 0 ] && break
    sleep 0.1
  done
  pgrep -u "$(id -u)" -f "^$1" | wc -l | tr -d ' '
}

# session <commands> [directory] - feed the commands to a client, output in $OUT
# session <commands> [directory] - one client against a fresh server, which
# is then stopped. $alive says whether the server was still up after the
# client left, which since phase 3 it always should be
session() {
  start_server "${2:-$WORK}"
  printf '%b' "$1" | timeout 30 "$ROOT/myclient" > "$OUT" 2>&1
  client_status=$?
  if server_outlives_client; then alive=yes; else alive=no; fi
  stop_server
}

# the dates and sizes in ls -l can differ between two runs
normalise() {
  sed -E 's/[0-9]+:[0-9]+|[A-Z][a-z]{2} +[0-9]+//g; s/ +/ /g' "$1"
}

fixture() {
  rm -rf "$1" && mkdir -p "$1"
  printf 'beta\nalpha beta\nalpha\nbeta\n' > "$1"/in.txt
}

fixture "$WORK"
mkdir -p "$WORK"/local "$WORK"/remote
fixture "$WORK"/local
fixture "$WORK"/remote

# ---------------------------------------------------------------------------
echo "every phase 1 case gives the same result as myshell"
while IFS= read -r line || [ -n "$line" ]; do
  printf '%s\n' "$line" | (cd "$WORK"/local && "$ROOT/myshell")
done < tests/parser_tests.txt > "$WORK"/local.out 2>&1

session "$(cat tests/parser_tests.txt)\nexit\n" "$WORK"/remote

# only this message is different on purpose, the spec's client shows the name
sed 's/^Command not found: .*/Command not found./' "$OUT" > "$WORK"/remote.out
check "same output" "$(normalise "$WORK"/local.out)" "$(normalise "$WORK"/remote.out)"
check "same files created" "$(ls "$WORK"/local)" "$(ls "$WORK"/remote)"
check "same content in out.txt" "$(cat "$WORK"/local/out.txt)" "$(cat "$WORK"/remote/out.txt)"
check "client exits 0" 0 "$client_status"
check "server keeps running after the client leaves" yes "$alive"
echo

# ---------------------------------------------------------------------------
echo "large output"
session 'seq 1 2000000\nexit\n'
check "13 MB arrives unchanged" "$(seq 1 2000000 | cksum)" "$(cksum < "$OUT")"
session 'seq 1 100000\necho after\nexit\n'
check "next command still works" after "$(tail -1 "$OUT")"
echo

# ---------------------------------------------------------------------------
echo "what the commands inherit"
session 'yes | head -1\nexit\n'
check "yes | head -1 ends quietly" y "$(cat "$OUT")"
session 'cat\nwc -l\necho done\nexit\n'
check "reading stdin does not hang" "0 done" "$(tr -s ' \n' ' ' < "$OUT" | sed 's/^ //; s/ $//')"
session 'ls /proc/self/fd\nexit\n'
[ -d /proc/self/fd ] && check "sockets are not inherited" "0 1 2 3" "$(tr '\n' ' ' < "$OUT" | sed 's/ $//')"
echo

# ---------------------------------------------------------------------------
echo "server output from the figures in the spec"
mkdir -p "$WORK"/figure && echo x > "$WORK"/figure/file1.txt
session 'ls -l\nunknowncmd\nexit\n' "$WORK"/figure
# every line but the first now names the client and the address it came
# from, so the fixed part is matched and the address left as a wildcard
C='\[Client #1 - 127\.0\.0\.1:[0-9]+\]'
for expected in \
  '^\[INFO\] Server started, waiting for client connections\.\.\.$' \
  "^\[INFO\] Client #1 connected from 127\.0\.0\.1:[0-9]+\. Assigned to Thread-[0-9]+\.\$" \
  "^\[RECEIVED\] $C Received command: \"ls -l\"\$" \
  "^\[EXECUTING\] $C Executing command: \"ls -l\"\$" \
  "^\[OUTPUT\] $C Sending output to client:\$" \
  "^\[RECEIVED\] $C Received command: \"unknowncmd\"\$" \
  "^\[EXECUTING\] $C Executing command: \"unknowncmd\"\$" \
  "^\[ERROR\] $C Command not found: \"unknowncmd\"\$" \
  "^\[OUTPUT\] $C Sending error message to client: \"Command not found: unknowncmd\"\$" \
  "^\[INFO\] $C Client requested disconnect\. Closing connection\.\$" \
  '^\[INFO\] Client #1 disconnected\.$'; do
  check "$(printf '%.48s' "$(printf '%s' "$expected" | tr -d '\\^$')")" \
        1 "$(grep -cE "$expected" "$LOG")"
done
check "client shows the error" 1 "$(grep -cxF 'Command not found: unknowncmd' "$OUT")"
echo

# ---------------------------------------------------------------------------
echo "errors reach the client"
session 'ls /nosuchdir\necho "open\nls >\nexit\n'
check "error from a command"  1 "$(grep -c nosuchdir "$OUT")"
check "unclosed quote"        1 "$(grep -c '^Unclosed quote.$' "$OUT")"
check "missing file name"     1 "$(grep -c '^Output file not specified.$' "$OUT")"
echo

# ---------------------------------------------------------------------------
echo "a session ending leaves the server running"
session 'exit\n';        check "exit" yes "$alive"
session '   exit   \n';  check "exit with spaces" yes "$alive"
session 'exit now\n';    check "exit with an argument" yes "$alive"
session '"exit"\n';      check "quoted exit" yes "$alive"
session 'echo one\n';    check "client input ends" yes "$alive"

# a client killed while a command runs. yes never stops printing, so the failed
# send notices. sleep prints nothing, so the server notices when it ends
for cmd in 'sleep 2' 'yes'; do
  start_server "$WORK"
  (echo "$cmd"; sleep 2) | "$ROOT/myclient" > /dev/null 2>&1 &
  client=$!
  sleep 0.5
  kill -9 "$client"
  wait "$client" 2>/dev/null      # reap it so bash prints no job notice
  if server_outlives_client; then alive=yes; else alive=no; fi
  check "client killed during $cmd" yes "$alive"
  check "and $cmd is not left running" 0 "$(remaining "$cmd")"
  stop_server
done
echo

# ---------------------------------------------------------------------------
echo "a client that skips the client's own checks"
start_server "$WORK"
reply=$(python3 -c '
import socket, struct
s = socket.create_connection(("127.0.0.1", 8080))
s.sendall(struct.pack("!I", 60000) + b"a" * 60000)
n = struct.unpack("!I", s.recv(4, socket.MSG_WAITALL))[0]
print(s.recv(n, socket.MSG_WAITALL).decode().strip())
s.recv(4, socket.MSG_WAITALL)   # the empty message that ends the response
s.close()
')
check "60 KB command line is refused" "Command line too long." "$reply"
check "and the server is fine" yes "$(server_alive && echo yes || echo no)"
stop_server
echo

# ---------------------------------------------------------------------------
echo "starting up"
start_server "$WORK"
"$ROOT/myserver" > "$OUT" 2>&1
check "port in use exits 1" 1 "$?"
check "and says why" 1 "$(grep -c 'Address already in use' "$OUT")"
stop_server
echo

# ---------------------------------------------------------------------------
# the point of phase 3: one server, several clients, one after another. each
# gets its own number and the server is still there at the end
echo "several clients on one server"
start_server "$WORK"
printf 'echo one\nexit\n'  | timeout 20 "$ROOT/myclient" > "$WORK"/c1.out 2>&1
printf 'echo two\n'         | timeout 20 "$ROOT/myclient" > "$WORK"/c2.out 2>&1
printf 'echo three\nexit\n'| timeout 20 "$ROOT/myclient" > "$WORK"/c3.out 2>&1
sleep 0.4
check "first client served"  one   "$(cat "$WORK"/c1.out)"
check "second client served" two   "$(cat "$WORK"/c2.out)"
check "third client served"  three "$(cat "$WORK"/c3.out)"
check "each client gets its own number" 3 \
      "$(grep -cE '^\[INFO\] Client #[123] connected from ' "$LOG")"
check "each disconnect is logged against its client" 3 \
      "$(grep -cE '^\[INFO\] Client #[123] disconnected\.$' "$LOG")"
check "numbers are not reused" 1 "$(grep -cE 'Client #3 connected' "$LOG")"
check "server is still up after all three" yes \
      "$(server_alive && echo yes || echo no)"
stop_server
echo

rm -rf "$WORK" "$OUT" "$LOG"

echo "-----------------------------------------------"
printf '%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ] || exit 1