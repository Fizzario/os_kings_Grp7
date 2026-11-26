/*
demo.c : the program the scheduler schedules

stands in for a long running program so that preemption can be seen. it takes
the number of seconds of work to do as its one argument and prints its progress
once a second, so that watching the output is watching the scheduler decide

  ./demo 8     prints Demo 0/8 through Demo 8/8, taking 8 seconds

the first line is printed straight away and each later line one second after
the one before it, so a run of "demo n" takes n seconds and the value of n is
also the task's burst time. that is what lets the server treat the number in
the command as the predicted execution time without running anything first

nothing in here knows about the scheduler. the server stops a run with SIGSTOP
and resumes it with SIGCONT, which freezes and thaws this process wherever it
happens to be, so a resumed run carries on from the line it reached instead of
starting again. that is handled by the kernel and needs no support here
*/

#include <stdio.h>   // printf(), fprintf(), fflush()
#include <stdlib.h>  // strtol(), EXIT_SUCCESS, EXIT_FAILURE
#include <errno.h>   // errno, ERANGE
#include <limits.h>  // INT_MAX
#include <time.h>    // nanosleep(), struct timespec

// an hour of simulated work is far more than any test needs, and refusing
// anything larger keeps a mistyped argument from occupying the scheduler
// for the rest of the session
#define MAX_SECONDS 3600


/*
how finely the one second wait is cut up, in milliseconds

a pause costs the run whatever was left of the slice it was in when it
stopped, so twenty short waits lose at most a twentieth of a second where
one long one loses up to a whole second. see sleep_one_second below
*/
#define SLICE_MS 50


/*
sleeps for one second, in slices, so that being paused costs almost nothing

a run is paused with SIGSTOP and continued with SIGCONT. a sleep is not
suspended along with the process: the kernel works out when it should end
when it begins, and the time spent stopped counts against that, so a sleep
with half a second to go that is stopped for five seconds comes back and
returns at once. the half second of work it was in the middle of is gone,
and the program finishes that much sooner than the seconds it was asked for

there is no way to ask for a sleep that stops with the process, so the
second is cut into slices instead. a pause can only cost whatever was left
of the slice it interrupted, and the rest of the second is still waited for
afterwards, which keeps a paused run honest to within a fraction of a second

nanosleep rather than sleep throughout, because it reports what it had left
to the nanosecond where sleep reports it in whole seconds and would round a
part-finished slice away to nothing
*/
static void sleep_one_second(void) {
  int slices = 1000 / SLICE_MS;
  int i;

  for (i = 0; i < slices; i++) {
    struct timespec left = { 0, SLICE_MS * 1000000L }, rest;

    while (nanosleep(&left, &rest) == -1 && errno == EINTR)
      left = rest;
  }
}


/*
reads the argument as the number of seconds of work to do

returns the value, or -1 after printing why it was refused. strtol is used
rather than atoi because atoi cannot tell "0" from "abc", and a program that
silently did no work would look to the scheduler like one that finished
*/
static long read_seconds(const char *text) {
  char *end;
  long value;

  errno = 0;
  value = strtol(text, &end, 10);

  // end is left pointing at the first character that was not a digit, so an
  // empty string or trailing rubbish is caught here
  if (end == text || *end != '\0') {
    fprintf(stderr, "demo: not a number: %s\n", text);
    return -1;
  }

  if (errno == ERANGE || value < 0 || value > MAX_SECONDS) {
    fprintf(stderr, "demo: seconds must be between 0 and %d\n", MAX_SECONDS);
    return -1;
  }

  return value;
}


int main(int argc, char *argv[]) {
  long seconds, i;

  if (argc != 2) {
    fprintf(stderr, "usage: demo <seconds>\n");
    return EXIT_FAILURE;
  }

  seconds = read_seconds(argv[1]);
  if (seconds < 0)
    return EXIT_FAILURE;

  // i counts the seconds already done, so the run opens at 0 and closes at
  // seconds, and the gaps between the lines account for the whole burst
  for (i = 0; i <= seconds; i++) {
    printf("Demo %ld/%ld\n", i, seconds);

    // the server reads this through a pipe, and a pipe is not a terminal, so
    // stdout would be collected and written out in one block at exit without
    // this. the client has to see each line as it happens for the scheduling
    // to be visible at all, which the spec asks for explicitly
    fflush(stdout);

    // the last line is the end of the run, there is nothing left to wait for
    if (i < seconds)
      sleep_one_second();
  }

  return EXIT_SUCCESS;
}