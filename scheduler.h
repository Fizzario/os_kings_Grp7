/*
scheduler.h : the one thing that runs tasks

Only one task executes at a time across the whole server, which is the
single processor the phase asks us to simulate. Client threads no longer
run their own commands: each one turns its line into a task, submits it
here, and waits.

The scheduler runs on a thread of its own. It takes a task from the waiting
queue, gives it a turn, and either finishes with it or puts it back for
another turn later.

What is in this file is split in two on purpose, and the split is where the
work between us divides:

  the loop     takes a task, logs what it is doing, gives it a turn through
               the runner, and puts unfinished work back

  the policy   chooses which task to take, how long to give it, and whether
               an arriving task should interrupt the one running

Only the second part decides anything. Replacing it with the combined
algorithm the spec asks for does not touch the first.
*/

#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "task.h"


/*
Sets the scheduler up and starts its thread. listen_fd is handed on to
every task's child so that it can be closed there.

Returns 0, or -1 if the thread could not be started.
*/
int scheduler_start(int listen_fd);


/*
Hands a task over to be run, and returns at once.

The caller waits on the task's own semaphore for it to finish; it must not
touch the task again until then, because from here until it is posted the
task belongs to the scheduler.

Returns 0, or -1 if there is no room for it, in which case the task was not
accepted and nothing will post it.
*/
int scheduler_submit(task_t *t);


/*
Gives up on everything belonging to a client that has gone: queued tasks are
cancelled, and a program of theirs that is running or suspended is killed.

The client's own thread is the one that notices it has left, so this is
called on the way out of serving it.
*/
void scheduler_client_gone(const client_t *c);

#endif