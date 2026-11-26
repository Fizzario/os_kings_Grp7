/*
tasklog.h : the server's log for phase 4, and the execution summary

Phase 4 asks for a different log from phase 3's. It is denser, because with
tasks being started, paused and resumed there are many more lines, and every
one of them is prefixed with the client it belongs to:

  [5]<<< client connected
  [5]>>> pwd
  [5]--- created (-1)
  [5]--- started (-1)
  [5]<<< 44 bytes sent
  [5]--- ended (-1)

The number in brackets after a state is how much work the task has left. On
waiting it is what remained when the turn ended, on running what remains as
the turn begins. A shell task has no estimate and carries -1 throughout,
which is what the spec's own screenshots show.

started and running are the same event seen twice: a task beginning its
first turn is reported as started, one beginning any later turn as running.
That is the only difference between them.

As in phase 3, several threads write here at once, so every line is built
whole and written while one lock is held. An entry can never be cut in half
by another client's.

The execution summary is kept here too, since it is made of the same turns
the log reports:

  0)-P6-(3)-P7-(6)-P6-(13)-P7-(20)-P6-(22)-P7-(24)

One entry per turn, naming the task by its client and giving the seconds
elapsed when that turn ended. Only program tasks appear: a shell task is not
scheduled in turns, it runs once to completion.
*/

#ifndef TASKLOG_H
#define TASKLOG_H

#include <stddef.h>  // size_t
#include "task.h"


/*
The lines the server prints. One function per line, so that the wording of
the format lives in one file and cannot drift apart between the places that
need it.
*/

// the banner, printed once when the server comes up
void tasklog_server_started(void);

// [5]<<< client connected
void tasklog_client_connected(const client_t *c);

// [5]<<< client disconnected
void tasklog_client_disconnected(const client_t *c);

// [5]>>> pwd        the command as the client sent it
void tasklog_command(const client_t *c, const char *line);

// [5]--- created (12)      queued, not yet given the processor
void tasklog_created(const task_t *t);

// [5]--- started (12)      beginning its first turn
void tasklog_started(const task_t *t);

// [5]--- running (9)       beginning a later turn
void tasklog_running(const task_t *t);

// [5]--- waiting (9)       turn over, work still to do
void tasklog_waiting(const task_t *t);

// [5]--- ended (0)         will not run again
void tasklog_ended(const task_t *t);

// [5]<<< 134 bytes sent
void tasklog_bytes_sent(const task_t *t, size_t bytes);

// [5]!!! Command not found: nosuchcmd
void tasklog_error(const client_t *c, const char *text, size_t len);

// a bare line, to keep the log readable between one client's entries
void tasklog_blank(void);


/*
The execution summary.

summary_turn_begin starts the clock on the first program turn of a run, so
that the first entry is measured from the moment scheduling began rather
than from when the server was started.

summary_turn_end appends the turn that has just finished. summary_print
writes the line out and clears it, ready for the next batch of work.
*/
void summary_turn_begin(const task_t *t);
void summary_turn_end(const task_t *t);
void summary_print(void);

// true if any turn has been recorded and not yet printed
int summary_pending(void);

#endif