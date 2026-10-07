/*
executor.c : runs a parsed pipeline
*/

#include <stdio.h>   // printf()
#include "myshell.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <errno.h>

#define READ_END 0
#define WRITE_END 1
#define MAX_PIPES (MAX_CMDS - 1) // -1 because for N chained commands we need N-1 pipes


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
    int fd = open(outfile, O_WRONLY | O_CREAT | (O_TRUNC * !append_out) | (O_APPEND * append_out), 0644);
    if (fd == -1) {
      perror("open");
      _exit(1);
    }
    dup2(fd, STDOUT_FILENO);
    close(fd);
  }
  if (errfile != NULL) {
    int fd = open(errfile, O_WRONLY | O_CREAT | (O_TRUNC * !append_err) | (O_APPEND * append_err), 0644);
    if (fd == -1) {
      perror("open");
      _exit(1);
    }
    dup2(fd, STDERR_FILENO);
    close(fd);
  }
}


// closes all file descriptors in an array up to N 
static void close_fds(int fds[MAX_PIPES][2], int n) {
  for (int i = 0; i < MAX_PIPES && i < n; ++i) {
    close(fds[i][READ_END]);
    close(fds[i][WRITE_END]);
  }
}


// currently prints the pipeline, we need real execution
/*
To implement:
  - fork a child per command
  - dup2 the redirection files onto stdin, stdout and stderr
  - pipe() between consecutive commands
  - execvp the command in the child
  - parent waits for every child before returning
*/
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

  close_fds(fds, pl->ncmds - 1);
  for (int i = 0; i < pl->ncmds; ++i)
  {
    wait(NULL);
  }
}