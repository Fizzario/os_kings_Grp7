/*
executor.c : runs a parsed pipeline
*/

#include <stdio.h>   // printf()
#include "myshell.h"


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