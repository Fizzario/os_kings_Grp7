/*
myshell : basic unix shell

shows prompt
reads command line from user
splits line into tokens
will later hand tokens to parser.
*/

#include <stdio.h>    // printf(), fgets(), fflush()
#include <stdlib.h>   // EXIT_SUCCESS
#include <string.h>   // strcmp(), strcspn(), strtok()

#define MAX_LINE 1024  
#define MAX_TOKENS 128 



/*
splits line into tokens on spaces and tabs
tokens point into line itself, strtok writes terminators
returns token count or -1 
*/

static int tokenize(char *line, char *tokens[]) {
  int count = 0;

  // first call takes string, runs of whitespace are one separator
  char *t = strtok(line, " \t");

  while (t != NULL) {
    if (count >= MAX_TOKENS) return -1;
    tokens[count++] = t;
    t = strtok(NULL, " \t");
  }

  return count;
}


int main(void) {
  char line[MAX_LINE];
  char *tokens[MAX_TOKENS];
  int ntokens;
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

    ntokens = tokenize(line, tokens);

    // too many tokens
    if (ntokens < 0) {
      printf("Too many tokens in command line.\n");
      continue;
    }

    if (ntokens == 0) {
      continue;
    }

    // Temporary: printing tokens to confirm that splitting works
    for (i = 0; i < ntokens; i++) {
      printf("Token %d: [%s]\n", i, tokens[i]);
    }
  }

  return EXIT_SUCCESS;
}