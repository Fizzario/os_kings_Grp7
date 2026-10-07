/*
executor.c : runs a parsed pipeline
*/

#include <stdio.h>   // printf()
#include "myshell.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/types.h>


void redirection(char* infile, char* outfile, char* errfile, int append_out, int append_err) {
  if (infile != NULL) {
    int fd = open(infile, O_RDONLY);
    if (fd == -1) {
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
  int c;
  int i;

  for (c = 0; c < pl->ncmds; c++) {
    command_t *cmd = &pl->cmds[c];

    pid_t pid = fork();
    
    if (pid == -1) { // fork fails
      perror("fork");
      return;
    }
    else if (pid == 0) { // the child process
      redirection(cmd->infile,cmd->outfile,cmd->errfile,cmd->append_out, cmd->append_err);
      execvp(cmd->argv[0], cmd->argv);
      perror("exec");
      _exit(1);
    }
    else { // the parent process
      wait(NULL);
      return;
    }

    printf("Command %d:\n", c);
    printf("  Program: [%s]\n", cmd->argv[0]);
    
    for (i = 1; i < cmd->argc; i++) {
      printf("  Arg %d: [%s]\n", i, cmd->argv[i]);
    }
    
    if (cmd->infile != NULL) {
      printf("  Input: [%s]\n", cmd->infile);
    }
    
    if (cmd->outfile != NULL) {
      printf("  Output: [%s] append=%d\n", cmd->outfile, cmd->append_out);
    }

    if (cmd->errfile != NULL) {
      printf("  Error: [%s] append=%d\n", cmd->errfile, cmd->append_err);
    }
  }
}