#include <stdio.h>   // printf()
#include <string.h>  // strcmp(), strcpy()
#include "myshell.h"

#define MAX_TOKENS 128 

// operators end a word even with no space around them
static int tokenize(char *line, char *tokens[]) {

  // holds the text of every token
  static char store[MAX_TOKENS][MAX_LINE];

  int count = 0;
  int i = 0;
  char c;

  while (line[i] != '\0') {
    c = line[i];

    // spaces and tabs between tokens are skipped
    if (c == ' ' || c == '\t') {
      i++;
      continue;
    }

    if (count >= MAX_TOKENS) return -1;

    if (c == '<') {
      strcpy(store[count], "<");
      i += 1;
    }

    else if (c == '|') {
      strcpy(store[count], "|");
      i += 1;
    }

    else if (c == '>') {

      // a second > makes it the append form
      if (line[i + 1] == '>') {
        strcpy(store[count], ">>");
        i += 2;
      } else {
        strcpy(store[count], ">");
        i += 1;
      }
    }

    // 2> only counts here at the start of a token
    else if (c == '2' && line[i + 1] == '>') {

      if (line[i + 2] == '>') {
        strcpy(store[count], "2>>");
        i += 3;
      } else {
        strcpy(store[count], "2>");
        i += 2;
      }
    }

    else {

      // a normal word runs until a space or an operator character
      int j = 0;

      while (line[i] != '\0' && line[i] != ' ' && line[i] != '\t' &&
             line[i] != '<'  && line[i] != '>' && line[i] != '|') {
        store[count][j++] = line[i++];
      }

      store[count][j] = '\0';
    }

    tokens[count] = store[count];
    count++;
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
static int parse_command(char *tokens[], int start, int stop, command_t *cmd) {
  int i = start;
  char *t;

  init_command(cmd);

  while (i < stop) {
    t = tokens[i];

    if (strcmp(t, "<") == 0) {

        // missing filename
      if (i + 1 >= stop || is_operator(tokens[i + 1])) {
        printf("Input file not specified.\n");
        return -1;
      }

      cmd->infile = tokens[i + 1];
      i += 2;
    }

    else if (strcmp(t, ">") == 0 || strcmp(t, ">>") == 0) {

      if (i + 1 >= stop || is_operator(tokens[i + 1])) {
        printf("Output file not specified.\n");
        return -1;
      }

      cmd->outfile = tokens[i + 1];

      // >> appends, > overwrites
      cmd->append_out = (strcmp(t, ">>") == 0);
      i += 2;
    }

    else if (strcmp(t, "2>") == 0 || strcmp(t, "2>>") == 0) {

      if (i + 1 >= stop || is_operator(tokens[i + 1])) {
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



//parses line into pl, returns 0 or error msg
int parse_line(char *line, pipeline_t *pl) {
  char *tokens[MAX_TOKENS];
  int ntokens;
  int start = 0;
  int i;

  pl->ncmds = 0;

  ntokens = tokenize(line, tokens);


  if (ntokens < 0) {
    printf("Too many tokens in command line.\n");
    return -1;
  }

  // empty line, nothing to run
  if (ntokens == 0) {
    return 0;
  }

  // each | ends a segment, the tokens before it are one command
  for (i = 0; i <= ntokens; i++) {

    if (i == ntokens || strcmp(tokens[i], "|") == 0) {

      // a segment with no tokens at all means two pipes in a row,
      // or a pipe at the very start of the line
      if (i == start) {
        if (i == ntokens) {
          printf("Command missing after pipe.\n");
        } else {
          printf("Empty command between pipes.\n");
        }
        return -1;
      }

      if (pl->ncmds >= MAX_CMDS) {
        printf("Too many commands in pipeline.\n");
        return -1;
      }

      if (parse_command(tokens, start, i, &pl->cmds[pl->ncmds]) != 0) {
        return -1;
      }

      // a segment of only redirection, e.g. "ls | > out.txt"
      if (pl->cmds[pl->ncmds].argc == 0) {
        printf("Empty command between pipes.\n");
        return -1;
      }

      pl->ncmds++;
      start = i + 1;
    }
  }

  return 0;
}