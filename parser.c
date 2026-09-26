/*
parser.c : turns a command line into a command

splits the line into tokens
collects the tokens into a command.
*/

#include <stdio.h>   // printf()
#include <string.h>  // strtok()
#include "myshell.h"

#define MAX_TOKENS 128 

// splits lines into tokens, tokens point into line itself, strtok writes terminators
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


// fills cmd with tokens as arguments, returns 0 or -1 on too many arguments
static int parse_command(char *tokens[], int ntokens, command_t *cmd) {
  int i;

  cmd->argc = 0;

  for (i = 0; i < ntokens; i++) {

    // one slot stays free for the NULL at the end
    if (cmd->argc >= MAX_ARGS - 1) return -1;

    cmd->argv[cmd->argc++] = tokens[i];
  }

  // NULL is how execvp knows where arguments stop
  cmd->argv[cmd->argc] = NULL;

  return 0;
}



//parses line into cmd, returns 0 or error msg
int parse_line(char *line, command_t *cmd) {
  char *tokens[MAX_TOKENS];
  int ntokens;

  ntokens = tokenize(line, tokens);


  if (ntokens < 0) {
    printf("Too many tokens in command line.\n");
    return -1;
  }

  // empty line, nothing to run
  if (ntokens == 0) {
    cmd->argc = 0;
    cmd->argv[0] = NULL;
    return 0;
  }

  if (parse_command(tokens, ntokens, cmd) != 0) {
    printf("Too many arguments.\n");
    return -1;
  }

  return 0;
}