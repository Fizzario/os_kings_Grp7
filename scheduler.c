/*
scheduler.c : the loop that runs tasks, and the policy that chooses them

The loop is finished. The policy is not: what is here is plain round robin
with the two quanta, which the spec suggests starting from, and it is enough
to show tasks being given turns, suspended and resumed. What it does not do
is choose cleverly between waiting tasks, or interrupt a running task when a
better one turns up.

Everything the policy decides is gathered in one place below, marked, so
that the combined algorithm can replace it without the loop changing at all.
*/

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "queue.h"
#include "runner.h"
#include "tasklog.h"
#include "scheduler.h"


/*
The quanta, in seconds.

A short first turn lets a task that barely needs any time finish and get out
of the way, while a longer later turn stops a task that really does have
work to do from paying the cost of being swapped in and out over and over.
Three and seven are the values the spec itself suggests, and the sample runs
in it are consistent with them.
*/
#define QUANTUM_FIRST 3
#define QUANTUM_LATER 7


static queue_t queue;
static int     server_listen_fd = -1;

/*
The task holding the processor, and the lock that guards the pointer.

Only the scheduler thread writes it. Client threads read it when they submit
a task, to decide whether to ask for the running one to give way, and when
their client disconnects, to see whether the task being killed is theirs.
*/
static task_t         *running = NULL;
static pthread_mutex_t running_lock = PTHREAD_MUTEX_INITIALIZER;

/*
Set by a client thread to ask the running task to end its turn early, and
cleared by the scheduler once it has. sig_atomic_t because it is written by
one thread and read by another with nothing locked in between, which is all
a single flag needs.
*/
static volatile sig_atomic_t preempt_requested = 0;


/* ------------------------------------------------------------------ */
/* the policy                                                          */
/*                                                                     */
/* Everything the scheduler decides is in these three functions. The   */
/* loop below calls them and does not care how they answer.            */
/* ------------------------------------------------------------------ */

/*
Chooses which of the waiting tasks runs next.

Called with the queue's lock held, so it may only read the tasks it is
given. Returns an index into them, or -1 to wait for something better.

Plain round robin for now: whatever has been waiting longest. The combined
algorithm has to add, in order of priority

  - a waiting shell task goes before any program task, and several of them
    go in the order they arrived
  - otherwise the program task with the least work left
  - except the task that has just had a turn, if any other program task is
    ready to run
*/
static int pick_next(task_t *const *tasks, int count, void *ctx) {
  (void) tasks; (void) count; (void) ctx;
  return 0;
}


/*
How many seconds to give a task for the turn it is about to take.

Zero means no limit, which is how a shell task is run: the spec says that
once one has started it runs to completion and is never interrupted.
*/
static int quantum_for(const task_t *t) {
  if (t->kind == TASK_SHELL)
    return 0;

  return task_is_first_turn(t) ? QUANTUM_FIRST : QUANTUM_LATER;
}


/*
Decides whether a task arriving now should interrupt the one running.

Nothing is interrupted yet. The combined algorithm has to say yes when

  - a shell task arrives while a program task is running, since shell tasks
    have priority and a program can be put back
  - a program task arrives whose estimate is smaller than what the running
    program task has left

and no when a shell task is running, since a shell task is never
interrupted, whatever arrives behind it.
*/
static int should_preempt(const task_t *arriving, const task_t *current) {
  (void) arriving; (void) current;
  return 0;
}

/* ------------------------------------------------------------------ */
/* the loop                                                            */
/* ------------------------------------------------------------------ */

/*
Reports a task beginning a turn. The first turn of a task is started and
every later one is running, which is the only difference between them.
*/
static void log_turn_begin(const task_t *t) {
  if (task_is_first_turn(t))
    tasklog_started(t);
  else
    tasklog_running(t);
}


/*
Finishes with a task: says so, posts it, and prints the summary if this was
the last thing there was to do.
*/
static void finish(task_t *t) {
  tasklog_ended(t);
  tasklog_blank();

  /*
  The summary covers the tasks submitted so far, so it is printed once
  there is nothing left either running or waiting. Anything submitted after
  that starts a fresh line with its own clock.
  */
  if (queue_count(&queue) == 0)
    summary_print();

  // the thread that submitted this is waiting on it, and this is the only
  // thing that will ever release it
  sem_post(&t->done);
}


static void *scheduler_loop(void *arg) {
  (void) arg;

  while (1) {
    task_t    *t = queue_take(&queue, pick_next, NULL);
    run_result why;

    pthread_mutex_lock(&running_lock);
    running = t;
    preempt_requested = 0;   // cleared here, so a request aimed at the task
    pthread_mutex_unlock(&running_lock);   // that has just left is not kept

    summary_turn_begin(t);
    log_turn_begin(t);

    why = runner_turn(t, server_listen_fd, quantum_for(t), &preempt_requested);

    /*
    Recorded before the task is let go of. A turn is over whichever way it
    ended, and the summary is a record of turns rather than of tasks.
    */
    summary_turn_end(t);

    pthread_mutex_lock(&running_lock);
    running = NULL;
    pthread_mutex_unlock(&running_lock);

    if (why == RUN_DONE || why == RUN_LOST || why == RUN_FAILED) {
      finish(t);
      continue;
    }

    // the turn ended with work still to do, so it goes back for another one
    tasklog_waiting(t);

    if (queue_add(&queue, t) != 0) {
      // no room to put it back, which should not be possible: the task came
      // out of this queue, so its place in it was free a moment ago
      runner_kill(t);
      finish(t);
    }
  }

  return NULL;
}


int scheduler_start(int listen_fd) {
  pthread_t tid;

  queue_init(&queue);
  server_listen_fd = listen_fd;

  if (pthread_create(&tid, NULL, scheduler_loop, NULL) != 0) {
    perror("pthread_create");
    return -1;
  }

  // nothing ever joins it: the scheduler runs for as long as the server does
  pthread_detach(tid);
  return 0;
}


int scheduler_submit(task_t *t) {
  tasklog_created(t);

  if (queue_add(&queue, t) != 0)
    return -1;

  /*
  Asked after the task is queued, not before. If the running task gives way
  immediately, the scheduler must already be able to see what it is giving
  way to.
  */
  pthread_mutex_lock(&running_lock);
  if (running != NULL && should_preempt(t, running))
    preempt_requested = 1;
  pthread_mutex_unlock(&running_lock);

  return 0;
}


void scheduler_client_gone(const client_t *c) {
  queue_cancel_client(&queue, c);

  /*
  A program of theirs may be running or suspended rather than waiting, in
  which case it is not in the queue and cancelling it there does nothing.
  Asking for it to give way lets the scheduler reach it: the next thing the
  runner does is try to send its output to a socket that has closed, and
  the task is finished as lost.
  */
  pthread_mutex_lock(&running_lock);
  if (running != NULL && running->client == c)
    preempt_requested = 1;
  pthread_mutex_unlock(&running_lock);
}