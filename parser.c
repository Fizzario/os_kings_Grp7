/*
parser.c : turns a command line into a command

splits the line into tokens
collects the tokens into a command
pulls out the redirection files
*/

#include <stdio.h>   // printf()
#include <string.h>  // strtok(), strcmp()
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


// 1 if the token is a redirection operator, 0 otherwise
static int is_operator(const char *t) {
  return strcmp(t, "<")   == 0 || strcmp(t, ">")  == 0 ||
         strcmp(t, ">>")  == 0 || strcmp(t, "2>") == 0 ||
         strcmp(t, "2>>") == 0;
}


// clears cmd
static void init_command(command_t *cmd) {
  cmd->argc = 0;
  cmd->infile = NULL;
  cmd->outfile = NULL;
  cmd->append_out = 0;
  cmd->errfile = NULL;
  cmd->append_err = 0;
}


// fills cmd with tokens as arguments and redirection files, returns 0 or -1
static int parse_command(char *tokens[], int ntokens, command_t *cmd) {
  int i = 0;
  char *t;

  init_command(cmd);

  while (i < ntokens) {
    t = tokens[i];

    if (strcmp(t, "<") == 0) {

        // missing filename
      if (i + 1 >= ntokens || is_operator(tokens[i + 1])) {
        printf("Input file not specified.\n");
        return -1;
      }

      cmd->infile = tokens[i + 1];
      i += 2;
    }

    else if (strcmp(t, ">") == 0 || strcmp(t, ">>") == 0) {

      if (i + 1 >= ntokens || is_operator(tokens[i + 1])) {
        printf("Output file not specified.\n");
        return -1;
      }

      cmd->outfile = tokens[i + 1];

      // >> appends, > overwrites
      cmd->append_out = (strcmp(t, ">>") == 0);
      i += 2;
    }

    else if (strcmp(t, "2>") == 0 || strcmp(t, "2>>") == 0) {

      if (i + 1 >= ntokens || is_operator(tokens[i + 1])) {
        printf("Error output file not specified.\n");
        return -1;
      }

      cmd->errfile = tokens[i + 1];
      cmd->append_err = (strcmp(t, "2>>") == 0);
      i += 2;
    }

    else {

      //one slot stays free for the NULL at the end
      if (cmd->argc >= MAX_ARGS - 1) {
        printf("Too many arguments.\n");
        return -1;
      }

      cmd->argv[cmd->argc++] = t;
      i++;
    }
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
    init_command(cmd);
    cmd->argv[0] = NULL;
    return 0;
  }

  return parse_command(tokens, ntokens, cmd);
}