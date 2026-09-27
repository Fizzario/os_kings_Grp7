/*
myshell.h : shared types and limits

defines the command and pipeline structures, the parser entry point
and the executor entry point
shell loop, parser and executor can be compiled separately
*/

#ifndef MYSHELL_H
#define MYSHELL_H

#define MAX_LINE 1024  
#define MAX_ARGS 64    
#define MAX_CMDS 16    


// argv ends in NULL so it can go straight to execvp
typedef struct {
  char *argv[MAX_ARGS];  
  int argc;              // arg count

  char *infile;          // file after 
  char *outfile;         // file after > or >>
  int append_out;        // 1 if >> was used, 0 if >
  char *errfile;         // file after 2> or 2>>
  int append_err;        // 1 if 2>> was used, 0 if 2>
} command_t;


// commands joined by |
typedef struct {
  command_t cmds[MAX_CMDS];
  int ncmds;             
} pipeline_t;



//parses line into pl, returns 0 or error msg
int parse_line(char *line, pipeline_t *pl);


//runs an already parsed pipeline, waits for every command in it
void execute_pipeline(pipeline_t *pl);

#endif