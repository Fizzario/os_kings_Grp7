/*
myshell : basic unix shell

shows prompt
reads command line from user
hands the line to the parser
hands the parsed pipeline to the executor
*/

#include <stdio.h>    // printf(), fgets(), fflush()
#include <stdlib.h>   // EXIT_SUCCESS
#include <string.h>   // strcmp(), strcspn()
#include "myshell.h"


int main(void) {
  char line[MAX_LINE];
  pipeline_t pl;


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

    execute_pipeline(&pl);
  }

  return EXIT_SUCCESS;

}