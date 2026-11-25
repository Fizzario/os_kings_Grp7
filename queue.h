/*
queue.h : the tasks that are ready to run and are not running

Every client thread adds to this and the scheduler takes from it, so the
queue holds its own lock rather than asking its callers to remember one.

Taking the next task is not a pop. The scheduler does not want whichever
task arrived first, it wants to look at all of them and choose by how much
work each has left, so queue_take is given a function that does the
choosing. The queue keeps hold of the lock across that call, which means
the task a chooser picks cannot be cancelled between being looked at and
being removed. It also keeps the scheduling rules out of this file
completely: nothing here knows what a quantum is.

Order is preserved. Shell tasks have to be served in the order they
arrived, so removing a task from the middle shifts the rest down instead of
swapping the last one into the gap.
*/

#ifndef QUEUE_H
#define QUEUE_H

#include <pthread.h>
#include "task.h"


/*
A client's thread waits for its task to finish before reading another
command, so a client can never have more than one task waiting at a time
and the queue can never hold more than the number of connected clients.
Sixty four is far beyond what the tests put through it, and a full queue is
reported rather than quietly dropping the task.
*/
#define MAX_TASKS 64


typedef struct {
  task_t *tasks[MAX_TASKS];  // the ready tasks, oldest first
  int     count;

  pthread_mutex_t lock;

  /*
  Signalled whenever a task is added. A condition variable rather than a
  counting semaphore because a task can leave this queue without the
  scheduler taking it: a disconnecting client's tasks are cancelled where
  they stand. A count would then be one too high and the scheduler would
  wake to an empty queue, where waiting on a condition means re-checking
  that there is really something there.
  */
  pthread_cond_t not_empty;
} queue_t;


/*
Given the ready tasks, oldest first, returns the index of the one to run
next, or -1 if none of them should run yet.

Called with the queue's lock held, so it must only read the tasks it is
given and must not call back into the queue.
*/
typedef int (*task_chooser)(task_t *const *tasks, int count, void *ctx);


void queue_init(queue_t *q);
void queue_destroy(queue_t *q);


/*
Adds a ready task and wakes the scheduler.
Returns 0, or -1 if the queue is full.
*/
int queue_add(queue_t *q, task_t *t);


/*
Waits until the chooser picks a task, then removes and returns it.

Never returns NULL: a scheduler with nothing to do belongs asleep in here
rather than spinning. If the chooser declines everything the queue waits
for the next change and asks again.
*/
task_t *queue_take(queue_t *q, task_chooser choose, void *ctx);


/*
Removes every waiting task belonging to c, for a client that has gone.

Each one is marked cancelled and its waiting thread woken, so nothing is
left blocked on a task that will never run. Tasks that are already running
or suspended are not in this queue and are the scheduler's to deal with.

Returns how many were removed.
*/
int queue_cancel_client(queue_t *q, const client_t *c);


// how many tasks are waiting right now
int queue_count(queue_t *q);

#endif