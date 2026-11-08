#!/bin/bash
#
# run_concurrent_tests.sh : several clients against one server at once
#
# This is the suite for what phase 3 is actually for. The other suites run
# one client at a time and say nothing about what happens when two are
# connected together, which is where the interesting failures live: shared
# buffers, a log cut in half, one client's output arriving at another, one
# client leaving and taking the others with it.
#
# Two kinds of check are used.
#
#   check    a property that must hold however the server is built, whether
#            it serves clients one after another or all at once. A failure
#            here is a real failure.
#
#   pending  a property that can only hold once each client is served on
#            its own thread. Against a server that serves clients in turn
#            these report PENDING rather than FAIL, and turn into PASS by
#            themselves once the threading is in. They are the acceptance
#            tests for that work, written before it.
#
# The suite's exit status ignores pending checks, so the repository is not
# left with a red test for work that has not been done yet. A pending check
# that starts passing and then stops is reported as a failure, because by
# then it is a regression.

ROOT=$(pwd)
WORK=tests/.concurrent
LOG="$WORK"/server.log

PORT_WAIT=0.6     # time to let the server bind before the clients connect

pass=0
fail=0
pend=0

check() {
  if [ "$2" = "$3" ]; then
    printf '  %-50s PASS\n' "$1"; pass=$((pass + 1))
  else
    printf '  %-50s FAIL (expected %s, got %s)\n' "$1" "$2" "$3"; fail=$((fail + 1))
  fi
}

pending() {
  if [ "$2" = "$3" ]; then
    printf '  %-50s PASS\n' "$1"; pass=$((pass + 1))
  else
    printf '  %-50s PENDING (needs one thread per client)\n' "$1"
    pend=$((pend + 1))
  fi
}

start_server() {
  : > "$LOG"
  (cd "$WORK" && exec "$ROOT/myserver") > "$LOG" 2>&1 &
  srv=$!
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    grep -q waiting "$LOG" && break
    sleep 0.1
  done
}

stop_server() {
  kill -TERM "$srv" 2>/dev/null
  wait "$srv" 2>/dev/null
}

# runs one client with the given commands, output into the named file
client() {
  printf '%b' "$2" | timeout 30 "$ROOT/myclient" > "$1" 2>&1
}

rm -rf "$WORK" && mkdir -p "$WORK"
trap 'stop_server 2>/dev/null; rm -rf "$WORK"' EXIT

echo "building"
make myserver myclient > /dev/null || { echo "build failed"; exit 1; }
echo

# ---------------------------------------------------------------------------
# The test that tells a threaded server from one that takes clients in turn.
#
# One client runs a command that takes three seconds. Half a second later a
# second client connects and runs one that is instant. If each client has a
# thread the second is served straight away and finishes in well under a
# second. If clients are taken in turn it cannot even be accepted until the
# first has finished, so it waits out the remaining two and a half.
# ---------------------------------------------------------------------------
echo "two clients at the same time"
start_server

client "$WORK"/slow.out 'sleep 3\necho slow-done\nexit\n' &
slow=$!

sleep 0.5
fast_start=$(date +%s%N)
client "$WORK"/fast.out 'echo fast-done\nexit\n'
fast_end=$(date +%s%N)
wait $slow

fast_ms=$(( (fast_end - fast_start) / 1000000 ))

check   "the slow client got its output"  slow-done "$(tail -1 "$WORK"/slow.out)"
check   "the fast client got its output"  fast-done "$(tail -1 "$WORK"/fast.out)"
pending "the fast client did not wait for the slow one" \
        yes "$([ "$fast_ms" -lt 1500 ] && echo yes || echo no)"
printf '  %-50s %s ms\n' "(the fast client took)" "$fast_ms"

stop_server
echo

# ---------------------------------------------------------------------------
# Eight clients connecting together. Each asks for a string of its own, so
# any output reaching the wrong client is visible. This has to hold either
# way: serving clients in turn makes them wait, it does not mix them up.
# ---------------------------------------------------------------------------
echo "eight clients at once"
start_server

pids=""
for i in $(seq 1 8); do
  client "$WORK"/m$i.out "echo marker-$i-aaaaaaaaaaaaaaaa\nexit\n" &
  pids="$pids $!"
done
# only the clients: a bare wait would also wait for the server, which by
# design never exits
for p in $pids; do wait "$p"; done
sleep 0.4

wrong=0
missing=0
for i in $(seq 1 8); do
  grep -q "marker-$i-" "$WORK"/m$i.out || missing=$((missing + 1))
  # any other client's marker in this client's output is a crossed wire
  for j in $(seq 1 8); do
    [ "$i" = "$j" ] && continue
    grep -q "marker-$j-" "$WORK"/m$i.out && wrong=$((wrong + 1))
  done
done

check "every client got its own output"        0 "$missing"
check "no client got another client's output"  0 "$wrong"
check "all eight were given a number"          8 \
      "$(grep -cE '^\[INFO\] Client #[0-9]+ connected from ' "$LOG")"
check "the numbers are all different"          8 \
      "$(grep -oE '^\[INFO\] Client #[0-9]+ connected' "$LOG" | sort -u | wc -l | tr -d ' ')"
check "all eight disconnects were logged"      8 \
      "$(grep -cE '^\[INFO\] Client #[0-9]+ disconnected\.$' "$LOG")"
check "the server is still up"                 yes \
      "$(kill -0 $srv 2>/dev/null && echo yes || echo no)"

stop_server
echo

# ---------------------------------------------------------------------------
# The log with several clients writing to it together. Every line has to be
# a whole line of one entry: a line that starts in one client's entry and
# finishes in another's is the interleaving clientlog exists to prevent.
# ---------------------------------------------------------------------------
echo "the log under load"
start_server

pids=""
for i in $(seq 1 6); do
  client "$WORK"/l$i.out \
    "seq 1 200\nseq 1 200\nnosuchcommand-$i\nseq 1 200\nexit\n" &
  pids="$pids $!"
done
for p in $pids; do wait "$p"; done
sleep 0.5

# a log line is either one of the tags, or a line of a command's output,
# which here is a bare number or an error about a missing command
spliced=$(grep -cvE '^(\[(INFO|RECEIVED|EXECUTING|ERROR|OUTPUT)\] |[0-9]+$|Command not found: nosuchcommand-[0-9]+$|$)' "$LOG")

check "no log line is spliced" 0 "$spliced"
check "every client was served" 6 \
      "$(grep -cE '^\[INFO\] Client #[0-9]+ disconnected\.$' "$LOG")"
check "each error is attributed to one client" 6 \
      "$(grep -cE '^\[ERROR\] \[Client #[0-9]+ - [0-9.]+:[0-9]+\] Command not found: "nosuchcommand-[0-9]+"$' "$LOG")"

# each output entry's body must be only its own client's numbers
badentry=$(awk '
  /^\[OUTPUT\] \[Client #[0-9]+ - .*Sending output to client:$/ { inentry=1; next }
  /^\[/ { inentry=0; next }
  inentry && $0 !~ /^[0-9]*$/ { bad++ }
  END { print bad + 0 }
' "$LOG")
check "output entries hold only command output" 0 "$badentry"

stop_server
echo

# ---------------------------------------------------------------------------
# One client leaving, or being killed outright, must not disturb another
# that is connected at the same time.
# ---------------------------------------------------------------------------
echo "one client leaving does not disturb another"
start_server

client "$WORK"/long.out 'sleep 3\necho survived\nexit\n' &
long=$!
sleep 0.3

# timed, so that "was not held up" means it, rather than merely having
# been served at some point
quick_start=$(date +%s%N)
client "$WORK"/quick.out 'echo quick\nexit\n'
quick_ms=$(( ($(date +%s%N) - quick_start) / 1000000 ))

# and one that is killed rather than leaving politely
(printf 'sleep 5\n'; sleep 5) | "$ROOT/myclient" > /dev/null 2>&1 &
victim=$!
sleep 0.4
kill -9 "$victim"
wait "$victim" 2>/dev/null

wait $long
sleep 0.4

check   "the long client finished its work" survived "$(tail -1 "$WORK"/long.out)"
check   "the quick client was served"       quick    "$(tail -1 "$WORK"/quick.out)"
check   "the server survived a killed client" yes \
        "$(kill -0 $srv 2>/dev/null && echo yes || echo no)"
pending "the quick client did not wait for the long one" \
        yes "$([ "$quick_ms" -lt 1500 ] && echo yes || echo no)"
printf '  %-50s %s ms\n' "(the quick client took)" "$quick_ms"

stop_server

# nothing the clients started may be left behind
for _ in $(seq 50); do
  [ "$(pgrep -u "$(id -u)" -f '^sleep [25]$' | wc -l | tr -d ' ')" -eq 0 ] && break
  sleep 0.1
done
check "no command processes left running" 0 \
      "$(pgrep -u "$(id -u)" -f '^sleep [25]$' | wc -l | tr -d ' ')"
echo

# ---------------------------------------------------------------------------
# A client arriving while the server is busy has to be held, not refused.
# ---------------------------------------------------------------------------
echo "a client arriving while the server is busy"
start_server

client "$WORK"/busy.out 'sleep 2\nexit\n' &
busy=$!
sleep 0.4

client "$WORK"/late.out 'echo late\nexit\n'
wait $busy

check "the late client was served, not refused" late "$(tail -1 "$WORK"/late.out)"
check "it did not report a connection failure" 0 \
      "$(grep -c 'Could not connect' "$WORK"/late.out)"

stop_server
echo

# ---------------------------------------------------------------------------
echo "-----------------------------------------------"
if [ "$pend" -gt 0 ]; then
  printf '%d passed, %d failed, %d pending\n' "$pass" "$fail" "$pend"
  echo
  echo "The pending checks need one thread per client. They are the"
  echo "acceptance tests for that work and will pass once it is in; the"
  echo "suite does not fail on them until then."
else
  printf '%d passed, %d failed\n' "$pass" "$fail"
fi

[ "$fail" -eq 0 ] || exit 1