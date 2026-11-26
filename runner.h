/*
runner.h : how a task is made to run, pause, carry on and finish

This is the mechanism the scheduler drives. Which task runs, and for how
long, is decided elsewhere; nothing here knows what a quantum is or why a
turn ended, only how to carry one out.

A turn is one spell on the processor. It ends when the task finishes, when
the time it was given runs out, or when the scheduler asks for it to stop.
A task that has not finished is left suspended with its remaining time
brought up to date, and the next turn picks it up where this one left off.

Pausing is SIGSTOP and continuing is SIGCONT, sent to the task's whole
process group. The kernel freezes the processes where they stand, so a
resumed program carries on from the point it had reached and nothing has to
be saved or restored here. The group matters: a command runs as a child of
the shell process, so signalling the shell process alone would leave the
command it started running on.
*/

#ifndef RUNNER_H
#define RUNNER_H

#include <signal.h>  // sig_atomic_t
#include "task.h"


// why a turn ended
typedef enum {
  RUN_DONE,       // the task finished and will not run again
  RUN_EXPIRED,    // the time it was given ran out; suspended, can resume
  RUN_PREEMPTED,  // the scheduler asked it to stop; suspended, can resume
  RUN_LOST,       // the client went away, so there is nowhere to send output
  RUN_FAILED      // the task could not be started at all
} run_result;


/*
Gives t the processor for up to seconds seconds, forwarding its output to
its client as it appears.

The task is started on its first turn and continued on later ones, so the
caller does not have to tell the two apart.

seconds of 0 or less means no limit, which is how a shell task is run: once
started it is never interrupted.

preempt, if given, is watched while the task runs. The scheduler sets it to
ask for the turn to end early, and it is checked often enough that a task
gives way promptly rather than at the end of its quantum.

The task's remaining time is reduced by the seconds actually spent running
before this returns, and its state is left as TASK_STOPPED or TASK_DONE.
*/
run_result runner_turn(task_t *t, int listen_fd, int seconds,
                       volatile sig_atomic_t *preempt);


/*
Ends a task that will not be allowed to finish, for a client that has gone.

A suspended process does not act on being killed until it is continued, so
this sends SIGCONT before SIGKILL. The child is then collected, so that a
long session does not fill up with processes nobody waited for.

Safe to call on a task that was never started.
*/
void runner_kill(task_t *t);

#endif