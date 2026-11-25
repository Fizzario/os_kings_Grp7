/*
queue.c : the waiting queue and the locking that makes it safe

One lock covers the array and the count, and is held for the whole of every
operation including the call out to a chooser. Holding it across that call
is deliberate: it is what stops a task being cancelled in the moment between
a chooser looking at it and the queue removing it.
*/

#include <string.h>   // memmove()
#include "queue.h"


/*
Takes the task at index i out of the array, closing the gap behind it.

The rest shift down rather than the last one being swapped into the hole,
because waiting shell tasks have to be served in the order they arrived and
a swap would put the newest where the oldest had been.
*/
static task_t *remove_at(queue_t *q, int i) {
  task_t *t = q->tasks[i];

  memmove(&q->tasks[i], &q->tasks[i + 1],
          (size_t) (q->count - i - 1) * sizeof q->tasks[0]);
  q->count--;

  return t;
}


void queue_init(queue_t *q) {
  q->count = 0;
  pthread_mutex_init(&q->lock, NULL);
  pthread_cond_init(&q->not_empty, NULL);
}


void queue_destroy(queue_t *q) {
  pthread_mutex_destroy(&q->lock);
  pthread_cond_destroy(&q->not_empty);
}


int queue_add(queue_t *q, task_t *t) {
  int full;

  pthread_mutex_lock(&q->lock);

  full = q->count == MAX_TASKS;
  if (!full) {
    t->state = TASK_READY;
    q->tasks[q->count++] = t;
  }

  /*
  Signalled inside the lock. A scheduler woken here cannot run until this
  thread releases the lock anyway, and signalling outside it would leave a
  window in which the task is queued and nothing has been told about it.
  */
  pthread_cond_signal(&q->not_empty);
  pthread_mutex_unlock(&q->lock);

  return full ? -1 : 0;
}


task_t *queue_take(queue_t *q, task_chooser choose, void *ctx) {
  task_t *t = NULL;
  int     i;

  pthread_mutex_lock(&q->lock);

  /*
  Waiting in a loop rather than on a single signal. A thread can come out of
  pthread_cond_wait without having been signalled at all, and in any case
  the task that caused the signal may already have been cancelled by the
  time this thread holds the lock, so the only safe thing is to look again.

  A chooser that declines everything is treated the same way as an empty
  queue: there is nothing to run yet, so wait for the next change.
  */
  while (t == NULL) {
    if (q->count > 0) {
      i = choose(q->tasks, q->count, ctx);

      if (i >= 0 && i < q->count)
        t = remove_at(q, i);
    }

    if (t == NULL)
      pthread_cond_wait(&q->not_empty, &q->lock);
  }

  pthread_mutex_unlock(&q->lock);
  return t;
}


int queue_cancel_client(queue_t *q, const client_t *c) {
  int removed = 0;
  int i = 0;
  task_t *t;

  pthread_mutex_lock(&q->lock);

  /*
  i is only stepped on when a task is kept. Removing one shifts the rest
  down, so the next task to look at has taken the index the removed one had.
  */
  while (i < q->count) {
    if (q->tasks[i]->client == c) {
      t = remove_at(q, i);
      t->state = TASK_CANCELLED;

      /*
      The thread that queued this task is waiting for it to finish and will
      wait forever if nothing tells it the task is gone. Posting here is
      what lets that thread tidy up and end.
      */
      sem_post(&t->done);
      removed++;
    }
    else {
      i++;
    }
  }

  pthread_mutex_unlock(&q->lock);
  return removed;
}


int queue_count(queue_t *q) {
  int n;

  pthread_mutex_lock(&q->lock);
  n = q->count;
  pthread_mutex_unlock(&q->lock);

  return n;
}