/*
tests/test_queue.c : checks the waiting queue

Three things are worth testing here and the rest follows from them: that
order survives a removal from the middle, since shell tasks have to be
served in the order they arrived; that a chooser really is given every
waiting task rather than just the head; and that a thread waiting in
queue_take is woken by a task arriving, since a scheduler that missed a
wake-up would sit still with work in front of it.

Cancelling is tested too, because a client thread blocked on a task that
has been cancelled is a thread that never ends.

  gcc -Wall -Wextra -g -I. -pthread -o tests/test_queue \
      tests/test_queue.c queue.c task.c clientlog.c
  ./tests/test_queue
*/

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include "queue.h"

static int failures = 0;

static void ok(const char *what, int cond) {
  printf("  %-54s %s\n", what, cond ? "PASS" : "FAIL");
  if (!cond) failures++;
}


// takes whatever arrived first, which is all the early tests need
static int choose_oldest(task_t *const *tasks, int count, void *ctx) {
  (void) tasks; (void) count; (void) ctx;
  return 0;
}


// the shape the real scheduler will have: look at every waiting task and
// take the one with the least work left
static int choose_shortest(task_t *const *tasks, int count, void *ctx) {
  int best = 0, i;

  (void) ctx;
  for (i = 1; i < count; i++)
    if (tasks[i]->remaining < tasks[best]->remaining)
      best = i;

  return best;
}


/*
Declines everything until it is allowed to accept, counting how many times
it was asked. Used to check that a queue_take whose chooser says no waits
for the next change and asks again, rather than returning nothing.
*/
struct decliner {
  queue_t *q;
  int      accept;
  int      asked;
  task_t * volatile got;
};

static int choose_when_allowed(task_t *const *tasks, int count, void *ctx) {
  struct decliner *d = ctx;

  (void) tasks; (void) count;
  d->asked++;

  return d->accept ? 0 : -1;
}

static void *take_thread(void *arg) {
  struct decliner *d = arg;

  d->got = queue_take(d->q, choose_when_allowed, d);
  return NULL;
}


static void make_client(client_t *c, int port) {
  struct sockaddr_in addr;

  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t) port);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  client_init(c, port, &addr);
}


/*
Adds one task after a short pause, so that the main thread reaches
queue_take first and has to wait for it.
*/
struct late_add {
  queue_t *q;
  task_t  *t;
};

static void *add_later(void *arg) {
  struct late_add *a = arg;

  usleep(200000);        // 200 ms, long enough that take is already waiting
  queue_add(a->q, a->t);
  return NULL;
}


int main(void) {
  queue_t  q;
  client_t c1, c2;
  task_t   t[5];

  queue_init(&q);
  make_client(&c1, 5);
  make_client(&c2, 6);

  printf("tasks come back in the order they arrived\n");
  {
    task_init(&t[0], &c1, "ls");
    task_init(&t[1], &c1, "pwd");
    task_init(&t[2], &c2, "whoami");

    queue_add(&q, &t[0]);
    queue_add(&q, &t[1]);
    queue_add(&q, &t[2]);

    ok("three waiting", queue_count(&q) == 3);
    ok("the first one added comes back first",
       queue_take(&q, choose_oldest, NULL) == &t[0]);
    ok("then the second", queue_take(&q, choose_oldest, NULL) == &t[1]);
    ok("then the third",  queue_take(&q, choose_oldest, NULL) == &t[2]);
    ok("and the queue is empty", queue_count(&q) == 0);

    task_destroy(&t[0]); task_destroy(&t[1]); task_destroy(&t[2]);
  }

  printf("\nthe chooser sees every waiting task, not just the head\n");
  {
    task_init(&t[0], &c1, "./demo 12");
    task_init(&t[1], &c1, "./demo 3");
    task_init(&t[2], &c2, "./demo 7");

    queue_add(&q, &t[0]);
    queue_add(&q, &t[1]);
    queue_add(&q, &t[2]);

    ok("the shortest is taken from the middle",
       queue_take(&q, choose_shortest, NULL) == &t[1]);
    ok("then the next shortest",
       queue_take(&q, choose_shortest, NULL) == &t[2]);
    ok("then the longest",
       queue_take(&q, choose_shortest, NULL) == &t[0]);

    task_destroy(&t[0]); task_destroy(&t[1]); task_destroy(&t[2]);
  }

  printf("\norder survives a removal from the middle\n");
  {
    task_init(&t[0], &c1, "./demo 9");
    task_init(&t[1], &c1, "./demo 1");   // taken out of the middle
    task_init(&t[2], &c2, "./demo 9");

    queue_add(&q, &t[0]);
    queue_add(&q, &t[1]);
    queue_add(&q, &t[2]);

    queue_take(&q, choose_shortest, NULL);        // removes t[1]

    // t[0] and t[2] have the same remaining time, so only arrival order
    // can decide which of them comes back first
    ok("the older of the two left is still first",
       queue_take(&q, choose_oldest, NULL) == &t[0]);
    ok("and the newer is still second",
       queue_take(&q, choose_oldest, NULL) == &t[2]);

    task_destroy(&t[0]); task_destroy(&t[1]); task_destroy(&t[2]);
  }

  printf("\na full queue is refused rather than overrunning\n");
  {
    static task_t many[MAX_TASKS + 1];
    int i, added = 0, refused = 0;

    for (i = 0; i < MAX_TASKS + 1; i++) {
      task_init(&many[i], &c1, "ls");
      if (queue_add(&q, &many[i]) == 0) added++;
      else refused++;
    }

    ok("exactly MAX_TASKS were accepted", added == MAX_TASKS);
    ok("and the one over was refused",    refused == 1);

    ok("cancelling empties it",
       queue_cancel_client(&q, &c1) == MAX_TASKS);

    for (i = 0; i < MAX_TASKS + 1; i++) task_destroy(&many[i]);
  }

  printf("\ncancelling takes one client's tasks and leaves the others\n");
  {
    int value = -1;

    task_init(&t[0], &c1, "ls");
    task_init(&t[1], &c2, "pwd");
    task_init(&t[2], &c1, "./demo 4");

    queue_add(&q, &t[0]);
    queue_add(&q, &t[1]);
    queue_add(&q, &t[2]);

    ok("both of that client's tasks go",
       queue_cancel_client(&q, &c1) == 2);
    ok("the other client's task stays", queue_count(&q) == 1);
    ok("it is the right one", queue_take(&q, choose_oldest, NULL) == &t[1]);

    ok("a cancelled task says so", t[0].state == TASK_CANCELLED);

    // the thread that queued it is waiting on this, and would wait forever
    // if cancelling did not post it
    ok("and its waiting thread is woken",
       sem_getvalue(&t[0].done, &value) == 0 && value == 1);

    task_destroy(&t[0]); task_destroy(&t[1]); task_destroy(&t[2]);
  }

  printf("\na scheduler with nothing to do waits for work to arrive\n");
  {
    struct late_add a;
    pthread_t tid;
    task_t   *got;

    task_init(&t[0], &c1, "./demo 2");
    a.q = &q;
    a.t = &t[0];

    pthread_create(&tid, NULL, add_later, &a);

    // blocks here until add_later runs, 200 ms from now
    got = queue_take(&q, choose_oldest, NULL);
    pthread_join(tid, NULL);

    ok("the task added while waiting is the one returned", got == &t[0]);
    task_destroy(&t[0]);
  }

  printf("\na chooser that declines keeps waiting instead of giving up\n");
  {
    struct decliner d;
    pthread_t tid;

    task_init(&t[0], &c1, "ls");    // queued, but declined at first
    task_init(&t[1], &c2, "pwd");   // added later, only to wake the waiter

    queue_add(&q, &t[0]);

    d.q = &q;
    d.accept = 0;
    d.asked = 0;
    d.got = NULL;
    pthread_create(&tid, NULL, take_thread, &d);

    usleep(200000);
    ok("a declined task leaves the taker waiting", d.got == NULL);
    ok("and it was asked at least once",           d.asked >= 1);

    // allowing it to accept, then adding a task so that something signals
    d.accept = 1;
    queue_add(&q, &t[1]);
    pthread_join(tid, NULL);

    ok("it asks again once something changes", d.asked >= 2);
    ok("and then takes the task it had refused", d.got == &t[0]);

    queue_cancel_client(&q, &c2);
    task_destroy(&t[0]); task_destroy(&t[1]);
  }

  queue_destroy(&q);

  printf("\n%s (%d failure%s)\n",
         failures ? "FAILURES" : "ALL PASS",
         failures, failures == 1 ? "" : "s");
  return failures != 0;
}