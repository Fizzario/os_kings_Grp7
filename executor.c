/*
executor.c : runs a parsed pipeline
*/

#include <stdio.h>      // fprintf(), perror()
#include <unistd.h>     // fork(), execvp(), pipe(), dup2(), close(), _exit(), STDIN/STDOUT/STDERR_FILENO
#include <fcntl.h>      // open(), O_RDONLY, O_WRONLY, O_CREAT, O_TRUNC, O_APPEND
#include <sys/wait.h>   // wait()
#include <sys/types.h>  // pid_t
#include <errno.h>      // errno, ENOENT
#include "myshell.h"

#define READ_END 0
#define WRITE_END 1
#define MAX_PIPES (MAX_CMDS - 1) // -1 because for N chained commands we need N-1 pipes


// points stdin, stdout and stderr at the files the parser recorded
// runs in the child after the pipe ends are connected, so a file overrides a pipe
// > and 2> truncate an existing file, >> and 2>> append to it; new files get mode 0644
//
// the list is walked in the order the redirections were written, exactly as a
// real shell does, and that order is what makes the compound cases behave:
// - "> a > b" opens a, then opens b over it, so a is still created and left empty
// - "2> err < missing" has err already on stderr when the input open fails, so
//   the message lands in the file rather than on the terminal
// - the first failure stops the walk, so nothing after it is opened
//
// on failure prints the error and ends the child, so the command never runs
static void redirection(command_t *cmd) {
  for (int i = 0; i < cmd->nredirects; ++i) {
    redirect_t *r = &cmd->redirects[i];
    int fd;
    int stream;

    if (r->type == RED_IN) {
      fd = open(r->file, O_RDONLY);
      stream = STDIN_FILENO;
    }
    else {
      // the append forms keep what is already in the file, the others truncate
      int append = (r->type == RED_APPEND || r->type == RED_ERRAPPEND);
      fd = open(r->file, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0644);

      // 2> and 2>> move stderr, > and >> move stdout
      stream = (r->type == RED_ERR || r->type == RED_ERRAPPEND)
                 ? STDERR_FILENO : STDOUT_FILENO;
    }

    if (fd == -1) {
      if (errno == ENOENT) // catch the missing file error specifically
        fprintf(stderr, "File not found.\n");
      else
        perror("open");
      _exit(1);
    }

    dup2(fd, stream);
    close(fd);
  }
}


// closes both ends of the first n pipes; n is capped at MAX_PIPES, the size of the array
static void close_fds(int fds[MAX_PIPES][2], int n) {
  for (int i = 0; i < MAX_PIPES && i < n; ++i) {
    close(fds[i][READ_END]);
    close(fds[i][WRITE_END]);
  }
}


// executes all commands in a pipeline via exec while chaining their inputs and outputs
// for N correct commands it:
// - creates N-1 pipes
// - creates N child processes via fork
// waits for all of them at the end
// in case any error happens all traces are cleaned up and function returns
void execute_pipeline(pipeline_t *pl) {
  int fds[MAX_PIPES][2];
  for (int i = 0; i < pl->ncmds - 1; ++i) {
    if (pipe(fds[i]) == -1) {
      perror("pipe");
      close_fds(fds, i);
      return;
    }
  }

  for (int c = 0; c < pl->ncmds; c++) {
    command_t *cmd = &pl->cmds[c];

    pid_t pid = fork();
    
    if (pid == -1) { // fork fails
      perror("fork");
      close_fds(fds, pl->ncmds - 1);
      for (int i = 0; i < c; ++i) {
        wait(NULL);
      }
      return;
    }
    else if (pid == 0) { // the child process
      if (c > 0) // the first command always reads from stdin
        dup2(fds[c - 1][READ_END], STDIN_FILENO);
      if (c < pl->ncmds - 1) // the last command always outputs to stdout
        dup2(fds[c][WRITE_END], STDOUT_FILENO);
      close_fds(fds, pl->ncmds - 1);
      // redirection takes precedence over pipes so it's applied last
      redirection(cmd);
      execvp(cmd->argv[0], cmd->argv);
      if (errno == ENOENT) { // catch the missing executable error specifically
        if (pl->ncmds == 1)
          fprintf(stderr, "Command not found.\n");
        else
          fprintf(stderr, "Command not found in pipe sequence.\n");
      }
      else
        perror("exec");
      _exit(1);
    }
  }

  // close all copies in the parent process so they don't block readers
  close_fds(fds, pl->ncmds - 1);
  for (int i = 0; i < pl->ncmds; ++i)
  {
    wait(NULL);
  }
}