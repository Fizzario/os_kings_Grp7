/*
runner.c : starting, pausing, continuing and collecting a task's processes

The shape of a running task is the same as in phase 2: a child is forked
with its stdout and stderr on pipes, it parses and executes the line exactly
as myshell would, and the parent reads the pipes and passes what it finds to
the client. The shell code underneath is untouched.

Two things are new.

The child puts itself in a process group of its own. A command runs as a
child of that child, so a signal sent to one process would stop the shell
and leave the command running; sent to the group it reaches both. This is
what a real shell does with a job, and for the same reason.

A turn is bounded. The parent waits on the pipes with a timeout rather than
reading until they close, so it notices when the time runs out or the
scheduler asks for the task to stop, and can suspend the task instead of
seeing it through to the end.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include "myshell.h"
#include "protocol.h"
#include "tasklog.h"
#include "runner.h"

#define READ_END  0
#define WRITE_END 1

/*
How long the parent waits on the pipes before looking up. It has to be
short enough that a preemption asked for by the scheduler is acted on
quickly, and long enough that a task producing no output is not costing
anything to watch. A tenth of a second is far below the second that is the
smallest unit any of the timing here is measured in.
*/
#define TICK_MS 100


/*
One read of output, no larger than one message.

Shared rather than per task, and safe because only one task runs at a time
and only the scheduler's thread ever reads: a suspended task is not reading
anything, so there is never a second user of this.
*/
static char out[MAX_MSG];


/*
Milliseconds between two readings of the monotonic clock.

Measured to the millisecond rather than to the second on purpose. Comparing
only the seconds fields makes a turn that began at 0.9 and ended at 3.1 look
like three seconds when it was barely two, which both cuts the turn short
and under-charges the task for it. Over several turns that drift is enough
to finish a task a whole second early.
*/
static long ms_between(const struct timespec *a, const struct timespec *b) {
  return (b->tv_sec - a->tv_sec) * 1000L
       + (b->tv_nsec - a->tv_nsec) / 1000000L;
}


// the same span as whole seconds, to the nearest second
static int seconds_between(const struct timespec *a, const struct timespec *b) {
  return (int) ((ms_between(a, b) + 500) / 1000);
}


static void now(struct timespec *t) {
  clock_gettime(CLOCK_MONOTONIC, t);
}


/*
Runs in the child: becomes its own process group, puts its output on the
pipes, and then does exactly what the shell does with a line.

Never returns, ending with _exit as the children in executor.c do.
*/
static void run_child(task_t *t, int listen_fd, int ofd, int efd) {
  pipeline_t pl;
  int devnull;

  /*
  A group of its own, so that the server can stop and continue this task
  without touching anything else. The command this process is about to
  start inherits the group, which is the point: stopping the group stops
  the command, where stopping this process alone would not.
  */
  setpgid(0, 0);

  // the server ignores SIGPIPE and exec keeps that; without this, yes | head
  // would have yes report a write error instead of simply being stopped
  signal(SIGPIPE, SIG_DFL);

  // a command has no use for the sockets, and one left open would hold the
  // port and the connection after everything else had finished with them
  close(listen_fd);
  close(t->client->fd);

  // the client cannot type into a running command, so anything reading
  // stdin would wait on the server's terminal for ever
  devnull = open("/dev/null", O_RDONLY);
  if (devnull != -1) {
    dup2(devnull, STDIN_FILENO);
    close(devnull);
  }

  dup2(ofd, STDOUT_FILENO);
  dup2(efd, STDERR_FILENO);
  close(ofd);
  close(efd);

  // the client's output names the command that was not found, the local
  // shell's does not
  report_command_name = 1;

  // a parse error has already gone to stderr, which is now the pipe, so the
  // client sees it like any other error
  if (parse_line(t->line, &pl) != 0 || pl.ncmds == 0)
    _exit(0);

  /*
  The same check myshell makes, so that "exit now" or an exit in quotes
  still ends the session. Only the parser can see it, and the parser only
  runs here, so the child reports it to the server as an exit status.
  */
  if (pl.ncmds == 1 && strcmp(pl.cmds[0].argv[0], "exit") == 0)
    _exit(EXIT_REQUESTED);

  execute_pipeline(&pl);
  _exit(0);
}


/*
Starts the task: two pipes and a fork.
Returns 0, or -1 after reporting what failed.
*/
static int start(task_t *t, int listen_fd) {
  int ofds[2], efds[2];
  pid_t pid;

  if (pipe(ofds) == -1) {
    perror("pipe");
    return -1;
  }
  if (pipe(efds) == -1) {
    perror("pipe");
    close(ofds[READ_END]);
    close(ofds[WRITE_END]);
    return -1;
  }

  fflush(stdout);   // anything still buffered would be copied into the child

  pid = fork();

  if (pid == -1) {
    perror("fork");
    close(ofds[READ_END]);  close(ofds[WRITE_END]);
    close(efds[READ_END]);  close(efds[WRITE_END]);
    return -1;
  }

  if (pid == 0) {
    close(ofds[READ_END]);
    close(efds[READ_END]);
    run_child(t, listen_fd, ofds[WRITE_END], efds[WRITE_END]);
  }

  /*
  Also set in the parent. Whichever of the two runs first, the group is in
  place before the parent can send the task a signal, and setting it twice
  to the same value does no harm.
  */
  setpgid(pid, pid);

  // our copies of the write ends have to go, or the reads below would never
  // see the end of the output
  close(ofds[WRITE_END]);
  close(efds[WRITE_END]);

  t->ofd = ofds[READ_END];
  t->efd = efds[READ_END];
  t->pipes_open = 2;

  t->pid = pid;
  return 0;
}


// closes whichever of the task's pipes are still open and forgets them
static void close_pipes(task_t *t) {
  if (t->ofd != -1) { close(t->ofd); t->ofd = -1; }
  if (t->efd != -1) { close(t->efd); t->efd = -1; }
  t->pipes_open = 0;
}


/*
Reads what is waiting on one pipe and passes it to the client.

Returns 1 if something was read, 0 at end of file, and -1 if the client has
gone. The descriptor is closed and forgotten at end of file, so that poll
stops being asked about it.
*/
static int drain(task_t *t, int *fd, int is_err) {
  ssize_t n = read(*fd, out, sizeof out);

  if (n <= 0) {
    // 0 is the child closing its end; an error here is treated the same
    // way, since there is nothing further to be read either way
    close(*fd);
    *fd = -1;
    t->pipes_open--;
    return 0;
  }

  if (is_err)
    tasklog_error(t->client, out, (size_t) n);

  if (send_msg(t->client->fd, out, (size_t) n) != 0)
    return -1;

  t->bytes_sent += (size_t) n;
  return 1;
}


// stops or continues every process of the task
static void signal_group(const task_t *t, int sig) {
  if (t->pid > 0)
    kill(-t->pid, sig);
}


/*
Takes whatever the task has already written but the parent has not yet
read, after it has been stopped.

Without this, output the task produced before it was suspended would sit in
the pipe until its next turn, and the client would see lines appear that
belong to a time when the task was not running.
*/
static void drain_pending(task_t *t) {
  struct pollfd fds[2];
  int n, i;

  while (t->pipes_open > 0) {
    n = 0;
    if (t->ofd != -1) { fds[n].fd = t->ofd; fds[n].events = POLLIN; n++; }
    if (t->efd != -1) { fds[n].fd = t->efd; fds[n].events = POLLIN; n++; }

    // 0 as the timeout: whatever is there now, and then stop
    if (n == 0 || poll(fds, (nfds_t) n, 0) <= 0)
      return;

    for (i = 0; i < n; i++) {
      if (!(fds[i].revents & (POLLIN | POLLHUP)))
        continue;

      if (fds[i].fd == t->ofd) {
        if (drain(t, &t->ofd, 0) < 0) return;
      }
      else if (fds[i].fd == t->efd) {
        if (drain(t, &t->efd, 1) < 0) return;
      }
    }
  }
}


run_result runner_turn(task_t *t, int listen_fd, int seconds,
                       volatile sig_atomic_t *preempt) {
  struct timespec began, current;
  struct pollfd   fds[2];
  int    status = 0;
  int    nfds, i;
  int    lost = 0;
  run_result why = RUN_EXPIRED;

  if (task_is_first_turn(t)) {
    if (start(t, listen_fd) != 0)
      return RUN_FAILED;
  }
  else {
    // carry on from exactly where the last turn left off
    signal_group(t, SIGCONT);
  }

  t->state = TASK_RUNNING;
  t->turns++;
  now(&began);

  while (1) {
    // every process of the task has closed its output, so it is finishing
    if (t->pipes_open == 0) {
      why = RUN_DONE;
      break;
    }

    if (lost) {
      why = RUN_LOST;
      break;
    }

    if (preempt != NULL && *preempt) {
      why = RUN_PREEMPTED;
      break;
    }

    now(&current);
    if (seconds > 0 && ms_between(&began, &current) >= seconds * 1000L) {
      why = RUN_EXPIRED;
      break;
    }

    nfds = 0;
    if (t->ofd != -1) { fds[nfds].fd = t->ofd; fds[nfds].events = POLLIN; nfds++; }
    if (t->efd != -1) { fds[nfds].fd = t->efd; fds[nfds].events = POLLIN; nfds++; }

    /*
    Waiting with a timeout rather than reading until the pipes close. A
    task producing no output for a while must not stop the server noticing
    that its time is up or that it has been asked to give way.
    */
    if (poll(fds, (nfds_t) nfds, TICK_MS) < 0) {
      if (errno == EINTR)
        continue;        // a signal arriving is not a reason to stop
      why = RUN_LOST;
      break;
    }

    for (i = 0; i < nfds; i++) {
      if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR)))
        continue;

      if (fds[i].fd == t->ofd && drain(t, &t->ofd, 0) < 0) lost = 1;
      else if (fds[i].fd == t->efd && drain(t, &t->efd, 1) < 0) lost = 1;
    }
  }

  now(&current);

  if (why == RUN_DONE) {
    /*
    The pipes have closed, so the task is on its way out. waitpid collects
    it, and without that every finished task would be left as a process
    nobody had waited for.
    */
    waitpid(t->pid, &status, 0);
    close_pipes(t);

    t->pid = -1;
    t->state = TASK_DONE;
    t->exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (t->remaining > 0)
      t->remaining = 0;

    if (!lost)
      tasklog_bytes_sent(t, t->bytes_sent);
  }
  else if (why == RUN_LOST) {
    runner_kill(t);
  }
  else {
    // the turn is over but the work is not: freeze it where it stands
    signal_group(t, SIGSTOP);

    /*
    Waiting until the task reports that it really has stopped.

    A signal is delivered when the kernel gets round to it, not when kill
    returns, so without this the turn could end with the task still running
    for a moment longer. The scheduler would then start the next task while
    this one was still going, which is the one thing a single processor is
    not allowed to do. WUNTRACED is what makes waitpid return for a child
    that has stopped rather than finished.
    */
    if (waitpid(t->pid, &status, WUNTRACED) == t->pid && !WIFSTOPPED(status)) {
      /*
      It finished in the moment between its time running out and the signal
      arriving. There is nothing left to stop, so it is done after all.
      */
      drain_pending(t);
      close_pipes(t);

      t->pid = -1;
      t->state = TASK_DONE;
      t->exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
      t->remaining = 0;

      if (!lost)
        tasklog_bytes_sent(t, t->bytes_sent);

      return RUN_DONE;
    }

    drain_pending(t);

    t->state = TASK_STOPPED;

    /*
    Charged for the seconds actually spent running, not for the lines it
    printed. A task stopped before its first whole second has done no
    measurable work and keeps the estimate it had.
    */
    if (t->remaining > 0) {
      t->remaining -= seconds_between(&began, &current);
      if (t->remaining < 0)
        t->remaining = 0;
    }
  }

  return why;
}


void runner_kill(task_t *t) {
  int status;

  if (t->pid > 0) {
    /*
    A stopped process does not act on being killed until it is continued,
    so the obvious single signal would leave it suspended for ever.
    */
    signal_group(t, SIGCONT);
    signal_group(t, SIGKILL);
    waitpid(t->pid, &status, 0);
    t->pid = -1;
  }

  close_pipes(t);
  t->state = TASK_DONE;
}