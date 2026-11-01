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

# 0 if the server stopped within 3 seconds, otherwise kills it and returns 1
server_stops() {
  for _ in $(seq 30); do
    if ! kill -0 "$srv" 2>/dev/null; then
      wait "$srv"
      server_status=$?
      return 0
    fi
    sleep 0.1
  done
  kill -9 "$srv"
  wait "$srv" 2>/dev/null
  return 1
}

# session <commands> [directory] - feed the commands to a client, output in $OUT
session() {
  start_server "${2:-$WORK}"
  printf '%b' "$1" | timeout 30 "$ROOT/myclient" > "$OUT" 2>&1
  client_status=$?
  if server_stops; then stopped=yes; else stopped=no; fi
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
check "server stops by itself" yes "$stopped"
check "server exits 0" 0 "$server_status"
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
for expected in \
  '[INFO] Server started, waiting for client connections...' \
  '[INFO] Client connected.' \
  '[RECEIVED] Received command: "ls -l" from client.' \
  '[EXECUTING] Executing command: "ls -l"' \
  '[OUTPUT] Sending output to client:' \
  '[RECEIVED] Received command: "unknowncmd" from client.' \
  '[EXECUTING] Executing command: "unknowncmd"' \
  '[ERROR] Command not found: "unknowncmd"' \
  '[OUTPUT] Sending error message to client: "Command not found: unknowncmd"'; do
  check "$(printf '%.50s' "$expected")" 1 "$(grep -cxF "$expected" "$LOG")"
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
echo "every way the session can end stops the server"
session 'exit\n';        check "exit" yes "$stopped"
session '   exit   \n';  check "exit with spaces" yes "$stopped"
session 'exit now\n';    check "exit with an argument" yes "$stopped"
session '"exit"\n';      check "quoted exit" yes "$stopped"
session 'echo one\n';    check "client input ends" yes "$stopped"

# a client killed while a command runs. yes never stops printing, so the failed
# send notices. sleep prints nothing, so the server notices when it ends
for cmd in 'sleep 2' 'yes'; do
  start_server "$WORK"
  (echo "$cmd"; sleep 2) | "$ROOT/myclient" > /dev/null 2>&1 &
  client=$!
  sleep 0.5
  kill -9 "$client"
  if server_stops; then stopped=yes; else stopped=no; fi
  check "client killed during $cmd" yes "$stopped"
  check "and $cmd is not left running" 0 "$(pgrep -u "$(id -u)" -f "^$cmd" | wc -l | tr -d ' ')"
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
server_stops
check "and the server is fine" 0 "$server_status"
echo

# ---------------------------------------------------------------------------
echo "starting up"
start_server "$WORK"
"$ROOT/myserver" > "$OUT" 2>&1
check "port in use exits 1" 1 "$?"
check "and says why" 1 "$(grep -c 'Address already in use' "$OUT")"
printf 'exit\n' | "$ROOT/myclient" > /dev/null 2>&1
server_stops
session 'echo again\nexit\n'
check "restarts straight after a session" again "$(cat "$OUT")"
echo

rm -rf "$WORK" "$OUT" "$LOG"

echo "-----------------------------------------------"
printf '%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ] || exit 1
