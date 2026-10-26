#!/bin/bash
#
# runs the client against the test server in tests/ and checks each result
#
# the test server is a stand in, not the real one: it answers with canned
# replies instead of running commands, which is what makes the results
# predictable enough to check automatically. what is being tested here is the
# client and the framing under it, not the shell
#
# every case is wrapped in timeout, because the failure that matters most on
# a socket is a HANG: a client waiting for a message that is never coming

PORT_WAIT=0.5     # time to let the server bind before the client connects
pass=0
fail=0

# ---------------------------------------------------------------------------
# check <description> <expected> <actual>
# ---------------------------------------------------------------------------
check() {
  if [ "$2" = "$3" ]; then
    printf '  %-46s PASS\n' "$1"
    pass=$((pass + 1))
  else
    printf '  %-46s FAIL (expected %s, got %s)\n' "$1" "$2" "$3"
    fail=$((fail + 1))
  fi
}

# ---------------------------------------------------------------------------
# session <commands>  - start the server, feed the commands to the client
# leaves the client output in $OUT and the server log in $LOG
# ---------------------------------------------------------------------------
OUT=tests/.client_out
LOG=tests/.server_log

session() {
  ./tests/testserver > "$LOG" 2>&1 &
  srv=$!
  sleep $PORT_WAIT
  printf '%b' "$1" | timeout 30 ./myclient > "$OUT" 2>&1
  client_status=$?
  wait $srv 2>/dev/null
  server_status=$?
}

echo "building"
make myclient tests/testserver > /dev/null || { echo "build failed"; exit 1; }
echo

# ---------------------------------------------------------------------------
echo "a session of ordinary commands"
session 'echo one\necho two\nexit\n'
check "client exits 0"            "0" "$client_status"
check "both commands reached the server" \
      "2" "$(grep -c 'RECEIVED.*echo' $LOG)"
check "both replies were printed" "2" "$(grep -c 'stub reply line 1' $OUT)"
check "server shut down on exit"  "1" "$(grep -c 'sent exit' $LOG)"
echo

# ---------------------------------------------------------------------------
echo "output larger than one message"
session 'big\nexit\n'
check "100 KB response arrives whole" "100000" "$(wc -c < $OUT | tr -d ' ')"
check "first byte of the response"    "S"      "$(head -c1 $OUT)"
check "last byte of the response"     "E"      "$(tail -c2 $OUT | head -c1)"

session 'huge\nexit\n'
check "3 MB response arrives whole"   "3000000" "$(wc -c < $OUT | tr -d ' ')"
echo

# ---------------------------------------------------------------------------
echo "responses of other shapes"
session 'quiet\necho after\nexit\n'
check "a command with no output does not hang" "0" "$client_status"
check "the next command still works" \
      "1" "$(grep -c 'stub reply line 1 for: echo after' $OUT)"

session 'slow\nexit\n'
check "a response sent in pieces is printed" "2" "$(grep -c '^piece ' $OUT)"

session 'big\necho after\nexit\n'
check "stream stays in sync after a large response" \
      "1" "$(grep -c 'stub reply line 1 for: echo after' $OUT)"
echo

# ---------------------------------------------------------------------------
echo "ending the session"
session 'echo one\nexit\n'
check "exit shuts the server down"       "1" "$(grep -c 'sent exit' $LOG)"

session 'echo one\n'                      # stdin ends, no exit typed
check "EOF also shuts the server down"   "1" "$(grep -c 'disconnected' $LOG)"

session '   exit   \n'
check "exit with spaces is recognised" \
      "1" "$(grep -c 'command: \"exit\"' $LOG)"
echo

# ---------------------------------------------------------------------------
echo "lines the client handles by itself"
session '\n\n   \necho only-this\nexit\n'
check "blank lines are not sent" \
      "1" "$(grep -c 'RECEIVED.*echo only-this' $LOG)"
check "nothing else was sent" \
      "2" "$(grep -c 'RECEIVED' $LOG)"

session "$(python3 -c "print('echo ' + 'x'*2000)")\nexit\n"
check "an over-long line is refused" \
      "1" "$(grep -c 'Command line too long' $OUT)"
check "and is not sent to the server" \
      "1" "$(grep -c 'RECEIVED' $LOG)"
echo

# ---------------------------------------------------------------------------
echo "the server is not there"
timeout 10 ./myclient < /dev/null > "$OUT" 2>&1
status=$?        # captured now; a check in between would overwrite it
check "connect failure is reported" \
      "1" "$(grep -c 'Could not connect' $OUT)"
check "client exits non-zero"  "1" "$status"
echo

# ---------------------------------------------------------------------------
echo "the server goes away mid-session"
./tests/testserver --one-shot > "$LOG" 2>&1 &
sleep $PORT_WAIT
printf 'echo one\necho two\necho three\n' | timeout 15 ./myclient > "$OUT" 2>&1
status=$?
wait 2>/dev/null
check "the first reply was printed" "1" "$(grep -c 'stub reply line 1' $OUT)"
check "the loss is reported"        "1" "$(grep -c 'Connection to the server was lost' $OUT)"
check "client exits 1, not killed by a signal" "1" "$status"
echo

rm -f "$OUT" "$LOG"

echo "-----------------------------------------------"
printf '%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ] || exit 1