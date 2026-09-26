/*
myshell.h : shared types and limits

defines the command structure and parser entry point
shell loop and the parser can be compiled separately
*/

#ifndef MYSHELL_H
#define MYSHELL_H

#define MAX_LINE 1024  
#define MAX_ARGS 64    


// argv ends in NULL so it can go straight to execvp
typedef struct {
  char *argv[MAX_ARGS];  
  int argc;              // arg count
} command_t;



//parses line into cmd, returns 0 or error msg
int parse_line(char *line, command_t *cmd);

#endif