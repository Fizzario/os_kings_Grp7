/*
myshell : basic unix shell

shows prompt
reads command line from user
hands the line to the parser
will later hand the command to the executor
*/

#include <stdio.h>    // printf(), fgets(), fflush()
#include <stdlib.h>   // EXIT_SUCCESS
#include <string.h>   // strcmp(), strcspn()
#include "myshell.h"


int main(void) {
  char line[MAX_LINE];
  pipeline_t pl;
  int c;
  int i;


  while (1) {
    printf("$ ");

    // we flush prompt because stdout is bufferred, 
    fflush(stdout);


    // when user presses ctrl+D fgets returns NULL, then print newline from prompt restart
    if (fgets(line, sizeof(line), stdin) == NULL) {
      printf("\n");
      break;
    }

    // fgets keeps newline from user, we find its position to replace with terminator
    line[strcspn(line, "\n")] = '\0';

    // "exit" terminates
    if (strcmp(line, "exit") == 0) {
      break;
    }

    // parse error, message already printed by the parser
    if (parse_line(line, &pl) != 0) {
      continue;
    }

    // empty line, nothing to run
    if (pl.ncmds == 0) {
      continue;
    }

    // Temporary: printing the pipeline to confirm that parsing works
    for (c = 0; c < pl.ncmds; c++) {
      command_t *cmd = &pl.cmds[c];

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

  return EXIT_SUCCESS;

}