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
// on failure prints the error and ends the child, so the command never runs
static void redirection(char* infile, char* outfile, char* errfile, int append_out, int append_err) {
  if (infile != NULL) {
    int fd = open(infile, O_RDONLY);
    if (fd == -1) {
      if (errno == ENOENT) // catch the missing file error specifically
        fprintf(stderr, "File not found.\n");
      else
        perror("open");
      _exit(1);
    }
    dup2(fd, STDIN_FILENO);
    close(fd);
  }
  if (outfile != NULL) {
    // the append flag picks O_APPEND (for >>) or O_TRUNC (for >)
    int fd = open(outfile, O_WRONLY | O_CREAT | (O_TRUNC * !append_out) | (O_APPEND * append_out), 0644);
    if (fd == -1) {
      perror("open");
      _exit(1);
    }
    dup2(fd, STDOUT_FILENO);
    close(fd);
  }
  if (errfile != NULL) {
    // same choice for 2>> and 2>
    int fd = open(errfile, O_WRONLY | O_CREAT | (O_TRUNC * !append_err) | (O_APPEND * append_err), 0644);
    if (fd == -1) {
      perror("open");
      _exit(1);
    }
    dup2(fd, STDERR_FILENO);
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
      redirection(cmd->infile,cmd->outfile,cmd->errfile,cmd->append_out, cmd->append_err);
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