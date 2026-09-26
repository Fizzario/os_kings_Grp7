/*
myshell : basic unix shell

shows prompt
reads command line from user
will later hand line to parser.
*/

#include <stdio.h>    // printf(), fgets(), fflush()
#include <stdlib.h>   // EXIT_SUCCESS
#include <string.h>   // strcmp(), strcspn()

#define MAX_LINE 1024  



int main(void) {
  char line[MAX_LINE];


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

    // empty lines just continue
    if (line[0] == '\0') {
      continue;
    }

    // Temporary: echoing line to confirm that reading works
    printf("Read command: [%s]\n", line);
  }

  return EXIT_SUCCESS;
}