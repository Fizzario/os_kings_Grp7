/*
parser.c : turns a command line into a pipeline_t

tokenizes the line, splits the tokens on |, and fills one command_t
per segment with its arguments and redirection files
*/

#include <stdio.h>   // fprintf()
#include <string.h>  // strcpy()
#include "myshell.h"

#define MAX_TOKENS 512 // enough for MAX_CMDS commands with arguments and redirections

// what a token is, decided once while scanning the line
// quoted text is always TOK_WORD, so "|" is an argument and not a pipe
typedef enum {
  TOK_WORD,       // an argument or a filename
  TOK_PIPE,       // |
  TOK_IN,         // <
  TOK_OUT,        // >
  TOK_APPEND,     // >>
  TOK_ERR,        // 2>
  TOK_ERRAPPEND   // 2>>
} tok_type;

// the two ways tokenizing can fail, returned instead of a token count
#define TOK_TOO_MANY -1
#define TOK_UNCLOSED -2


// splits line into tokens and records what each one is
// operators end a word even with no space around them, but a quote turns
// everything up to its partner into plain text and the quotes themselves
// are dropped, so "alpha beta" is one argument and "|" is not a pipe
// returns the token count, or TOK_TOO_MANY / TOK_UNCLOSED on failure
static int tokenize(char *line, char *tokens[], tok_type types[]) {

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

    if (count >= MAX_TOKENS) return TOK_TOO_MANY;

    if (c == '<') {
      strcpy(store[count], "<");
      types[count] = TOK_IN;
      i += 1;
    }

    else if (c == '|') {
      strcpy(store[count], "|");
      types[count] = TOK_PIPE;
      i += 1;
    }

    else if (c == '>') {

      // a second > makes it the append form
      if (line[i + 1] == '>') {
        strcpy(store[count], ">>");
        types[count] = TOK_APPEND;
        i += 2;
      } else {
        strcpy(store[count], ">");
        types[count] = TOK_OUT;
        i += 1;
      }
    }

    // 2> only counts here at the start of a token
    else if (c == '2' && line[i + 1] == '>') {

      if (line[i + 2] == '>') {
        strcpy(store[count], "2>>");
        types[count] = TOK_ERRAPPEND;
        i += 3;
      } else {
        strcpy(store[count], "2>");
        types[count] = TOK_ERR;
        i += 2;
      }
    }

    else {

      // a normal word runs until an unquoted space or operator character
      int j = 0;

      // the quote character we are inside, 0 when we are not in one
      char quote = 0;

      while (line[i] != '\0') {

        // inside quotes only the matching quote ends the run, so spaces
        // and operator characters are copied like any other letter
        if (quote != 0) {
          if (line[i] == quote) {
            quote = 0;
            i++;
          } else {
            store[count][j++] = line[i++];
          }
        }

        // an opening quote is dropped and turns quoting on
        else if (line[i] == '"' || line[i] == '\'') {
          quote = line[i];
          i++;
        }

        // unquoted, these end the word
        else if (line[i] == ' ' || line[i] == '\t' ||
                 line[i] == '<' || line[i] == '>' || line[i] == '|') {
          break;
        }

        else {
          store[count][j++] = line[i++];
        }
      }

      // the line ended before the closing quote arrived
      if (quote != 0) return TOK_UNCLOSED;

      store[count][j] = '\0';
      types[count] = TOK_WORD;
    }

    tokens[count] = store[count];
    count++;
  }

  return count;
}


// clears cmd
static void init_command(command_t *cmd) {
  cmd->argc = 0;
  cmd->nredirects = 0;
}


// appends one redirection to cmd, keeping the order it was written in
// returns 0, or -1 when the command already carries MAX_REDIRECTS of them
static int add_redirect(command_t *cmd, red_type type, char *file) {
  if (cmd->nredirects >= MAX_REDIRECTS) {
    fprintf(stderr, "Too many redirections.\n");
    return -1;
  }

  cmd->redirects[cmd->nredirects].type = type;
  cmd->redirects[cmd->nredirects].file = file;
  cmd->nredirects++;

  return 0;
}


// fills cmd with tokens as arguments and redirection files, returns 0 or -1
static int parse_command(char *tokens[], tok_type types[],
                         int start, int stop, command_t *cmd) {
  int i = start;

  init_command(cmd);

  while (i < stop) {

    // an operator takes the token after it as its filename, so that token
    // has to be there and has to be a word rather than another operator
    if (types[i] == TOK_IN) {

      if (i + 1 >= stop || types[i + 1] != TOK_WORD) {
        fprintf(stderr, "Input file not specified.\n");
        return -1;
      }

      if (add_redirect(cmd, RED_IN, tokens[i + 1]) != 0) return -1;
      i += 2;
    }

    else if (types[i] == TOK_OUT || types[i] == TOK_APPEND) {

      if (i + 1 >= stop || types[i + 1] != TOK_WORD) {
        fprintf(stderr, "Output file not specified.\n");
        return -1;
      }

      // >> appends, > overwrites
      if (add_redirect(cmd,
                       types[i] == TOK_APPEND ? RED_APPEND : RED_OUT,
                       tokens[i + 1]) != 0) return -1;
      i += 2;
    }

    else if (types[i] == TOK_ERR || types[i] == TOK_ERRAPPEND) {

      if (i + 1 >= stop || types[i + 1] != TOK_WORD) {
        fprintf(stderr, "Error output file not specified.\n");
        return -1;
      }

      if (add_redirect(cmd,
                       types[i] == TOK_ERRAPPEND ? RED_ERRAPPEND : RED_ERR,
                       tokens[i + 1]) != 0) return -1;
      i += 2;
    }

    else {

      //one slot stays free for the NULL at the end
      if (cmd->argc >= MAX_ARGS - 1) {
        fprintf(stderr, "Too many arguments.\n");
        return -1;
      }

      cmd->argv[cmd->argc++] = tokens[i];
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
  tok_type types[MAX_TOKENS];
  int ntokens;
  int start = 0;
  int i;

  pl->ncmds = 0;

  ntokens = tokenize(line, tokens, types);


  if (ntokens == TOK_TOO_MANY) {
    fprintf(stderr, "Too many tokens in command line.\n");
    return -1;
  }

  if (ntokens == TOK_UNCLOSED) {
    fprintf(stderr, "Unclosed quote.\n");
    return -1;
  }

  // empty line, nothing to run
  if (ntokens == 0) {
    return 0;
  }

  // each | ends a segment, the tokens before it are one command
  for (i = 0; i <= ntokens; i++) {

    if (i == ntokens || types[i] == TOK_PIPE) {

      // a segment with no tokens at all means two pipes in a row,
      // or a pipe at the very start of the line
      if (i == start) {
        if (i == ntokens) {
          fprintf(stderr, "Command missing after pipe.\n");
        } else {
          fprintf(stderr, "Empty command between pipes.\n");
        }
        return -1;
      }

      if (pl->ncmds >= MAX_CMDS) {
        fprintf(stderr, "Too many commands in pipeline.\n");
        return -1;
      }

      if (parse_command(tokens, types, start, i, &pl->cmds[pl->ncmds]) != 0) {
        return -1;
      }

      // a segment of only redirection, e.g. "ls | > out.txt"
      if (pl->cmds[pl->ncmds].argc == 0) {
        fprintf(stderr, "Empty command between pipes.\n");
        return -1;
      }

      pl->ncmds++;
      start = i + 1;
    }
  }

  return 0;
}