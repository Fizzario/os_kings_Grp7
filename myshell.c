/*
myshell : basic unix shell

shows prompt
reads command line from user
splits line into tokens
builds a command from the tokens
will later hand the command to the executor
*/

#include <stdio.h>    // printf(), fgets(), fflush()
#include <stdlib.h>   // EXIT_SUCCESS
#include <string.h>   // strcmp(), strcspn(), strtok()

#define MAX_LINE 1024  
#define MAX_TOKENS 128 
#define MAX_ARGS 64    



typedef struct {
  char *argv[MAX_ARGS]; 
  int argc;              // how many arguments, not counting NULL
} command_t;


/*
splits line into tokens on spaces and tabs
tokens point into line itself, strtok writes terminators
returns token count or -1 
*/

static int tokenize(char *line, char *tokens[]) {
  int count = 0;

  // First call takes string, runs of whitespace are one separator
  char *t = strtok(line, " \t");

  while (t != NULL) {
    if (count >= MAX_TOKENS) return -1;
    tokens[count++] = t;
    t = strtok(NULL, " \t");
  }

  return count;
}


// fills cmd with tokens as args
static int parse_command(char *tokens[], int ntokens, command_t *cmd) {
  int i;

  cmd->argc = 0;

  for (i = 0; i < ntokens; i++) {

    // one slot is free for NULL
    if (cmd->argc >= MAX_ARGS - 1) return -1;

    cmd->argv[cmd->argc++] = tokens[i];
  }

  // NULL ends arguments list
  cmd->argv[cmd->argc] = NULL;

  return 0;
}


int main(void) {
  char line[MAX_LINE];
  char *tokens[MAX_TOKENS];
  command_t cmd;
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

    // too many arguments
    if (parse_command(tokens, ntokens, &cmd) != 0) {
      printf("Too many arguments.\n");
      continue;
    }

    // Temporary: printing the command to confirm that parsing works
    printf("Program: [%s]\n", cmd.argv[0]);
    for (i = 1; i < cmd.argc; i++) {
      printf("  Arg %d: [%s]\n", i, cmd.argv[i]);
    }
  }

  return EXIT_SUCCESS;

}