/*
task.h : what a client's request becomes once the server has received it

In phase 3 a client thread ran its own commands. From phase 4 it does not:
only one task may execute at a time across the whole server, so a thread
hands its work over and waits. A task is what gets handed over, and it
carries everything the scheduler needs in order to choose it.

There are two kinds, and the difference is whether a task can be interrupted:

  a shell task   is an ordinary command or pipeline. Once it starts it runs
                 to completion, and it has no useful estimate of how long it
                 will take, so it carries -1 where a time would go

  a program task is a program run such as ./demo 12. It can be stopped part
                 way through and continued later from the same point, so the
                 scheduler may give it the processor in several turns

This header is the interface both halves of the project build against: the
queue, the log and the scheduler all speak in terms of task_t, and none of
them needs to know how the others work.
*/

#ifndef TASK_H
#define TASK_H

#include <stddef.h>     // size_t
#include <sys/types.h>  // pid_t
#include <semaphore.h>  // sem_t
#include "myshell.h"    // MAX_LINE
#include "clientlog.h"  // client_t


/*
The burst given to a program whose running time cannot be read from the
command, which the spec allows the scheduler to assign for itself.

Five seconds sits deliberately between the two quanta. It is longer than the
first turn's three seconds, so an unknown program cannot win the shortest
remaining time comparison against work that really is short, and shorter than
the seven seconds of a later turn, so one that finishes quickly still gets
out of the way within two turns.
*/
#define DEFAULT_BURST 5


/*
The exit status a task's child uses to say the line was an exit command.

myclient trims exit before sending it, so the plain comparison in the server
catches the usual case without running anything. "exit now", or exit inside
quotes, is only recognisable after parsing, which happens in the child, so
the child reports it this way instead.

execute_pipeline returns nothing and the child otherwise exits 0, so a
command's own status cannot be mistaken for this.
*/
#define EXIT_REQUESTED 42


// what the line turned out to be, which decides whether it can be preempted
typedef enum {
  TASK_SHELL,    // a command or pipeline: runs to completion once started
  TASK_PROGRAM   // a program run: scheduled in turns, stoppable, resumable
} task_kind;


/*
Where a task is in its life. The log's wording is not taken from this
directly: a task beginning its first turn is reported as started and one
beginning a later turn as running, and that difference comes from the number
of turns it has already had rather than from the state.
*/
typedef enum {
  TASK_READY,     // waiting in the queue for a turn
  TASK_RUNNING,   // the one task currently executing
  TASK_STOPPED,   // begun, suspended part way, waiting for another turn
  TASK_DONE,      // finished, successfully or not
  TASK_CANCELLED  // its client disconnected before it could finish
} task_state;


typedef struct {
  int       id;      // serial within one run, so the queue can remove by identity
  client_t *client;  // who it belongs to; the log names a task by this client
  task_kind kind;

  /*
  The command, copied rather than pointed at. The session's receive buffer is
  reused by the next command from that client, and a queued task may sit here
  for a good while before it runs, by which time the buffer holds something
  else entirely.
  */
  char line[MAX_LINE];

  int burst;      // seconds the task is predicted to need, -1 for a shell task
  int remaining;  // seconds still to do, -1 for a shell task
  int turns;      // execution turns taken so far, 0 before the first one

  task_state state;
  pid_t      pid;  // the child running it, -1 while nothing is running

  /*
  The read ends of the child's output, and how many of the two have yet to
  reach end of file. They belong to the task rather than to the runner
  because a suspended task keeps its pipes: its output is still sitting in
  them, waiting to be read when it is given another turn. A single set
  shared by whatever happens to be running would be overwritten the moment
  another task started, and the suspended one would come back to read from
  somebody else's pipes.
  */
  int ofd;
  int efd;
  int pipes_open;

  size_t bytes_sent;  // output passed to the client, for the log's sent line
  int    exit_status; // what the child exited with, -1 if it did not finish

  /*
  Posted once the task will not run again, whether it finished or was
  cancelled. The client thread waits on this instead of executing anything
  itself, which is what keeps one command per client in flight at a time.
  */
  sem_t done;
} task_t;


/*
Works out what a line is without parsing it.

The parser cannot be used here. It keeps its tokens in storage shared across
the whole process, which is safe only because parsing happens inside the
forked child; calling it from a client thread would have two clients writing
over each other's tokens. Deciding whether a line is a program run needs far
less than a parse, so this reads the line directly.

Returns the kind, and writes the estimated burst through burst: -1 for a
shell task, n for ./demo n, and DEFAULT_BURST for any other program.
*/
task_kind task_classify(const char *line, int *burst);


/*
Fills t in for a line just received from c, taking the next task number.
Returns 0, or -1 if the task's completion semaphore could not be created.
*/
int task_init(task_t *t, client_t *c, const char *line);


// releases what task_init set up; does not touch the client
void task_destroy(task_t *t);


/*
True while the task has yet to take a turn, which is what the log uses to
choose between reporting it as started and as running, and the scheduler to
choose between the first quantum and the longer one.
*/
int task_is_first_turn(const task_t *t);

#endif