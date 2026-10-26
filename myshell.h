/*
myshell.h : shared types and limits

defines the command and pipeline structures, the parser entry point
and the executor entry point
shell loop, parser and executor can be compiled separately
*/

#ifndef MYSHELL_H
#define MYSHELL_H

#define MAX_LINE 1024      // longest command line, including the newline
#define MAX_ARGS 64        // arguments per command, including argv[0]
#define MAX_CMDS 64        // commands in one pipeline
#define MAX_REDIRECTS 16   // redirections on one command


// which stream a redirection points at, and whether it appends
typedef enum {
  RED_IN,         // <
  RED_OUT,        // >
  RED_APPEND,     // >>
  RED_ERR,        // 2>
  RED_ERRAPPEND   // 2>>
} red_type;


// one redirection, e.g. the "> out.txt" part of a command
typedef struct {
  red_type type;
  char *file;
} redirect_t;


// argv ends in NULL so it can go straight to execvp
typedef struct {
  char *argv[MAX_ARGS];
  int argc;              // arg count

  // every redirection on this command, kept in the order it was written
  // the order is the meaning: each one is opened and applied in turn, so a
  // second > still creates the first file, and an earlier 2> is already in
  // place to catch the message when a later one fails
  redirect_t redirects[MAX_REDIRECTS];
  int nredirects;        // number of entries in redirects
} command_t;


// commands joined by |
typedef struct {
  command_t cmds[MAX_CMDS];
  int ncmds;             // number of commands in cmds
} pipeline_t;



//parses line into pl, returns 0 or error msg
int parse_line(char *line, pipeline_t *pl);


//runs an already parsed pipeline, waits for every command in it
void execute_pipeline(pipeline_t *pl);

#endif