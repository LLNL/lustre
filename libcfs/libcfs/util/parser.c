/*
 * Copyright (C) 2001 Cluster File Systems, Inc.
 *
 *   This file is part of Lustre, http://www.sf.net/projects/lustre/
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <libcfs/libcfsutil.h>

static command_t * top_level;           /* Top level of commands, initialized by
                                    * InitParser                              */
static char * parser_prompt = NULL;/* Parser prompt, set by InitParser      */
static int done;                   /* Set to 1 if user types exit or quit   */
static int ignore_errors;       /* Normally, the parser will quit when
                                   an error occurs in non-interacive
                                   mode. Setting this to non-zero will
                                   force it to keep buggering on. */


/* static functions */
static char *skipwhitespace(char *s);
static char *skiptowhitespace(char *s);
static command_t *find_cmd(char *name, command_t cmds[], char **next);
static int process(char *s, char **next, command_t *lookup, command_t **result,
                   char **prev);
static void print_commands(char *str, command_t *table);

static char * skipwhitespace(char * s)
{
        char * t;
        int    len;

        len = (int)strlen(s);
        for (t = s; t <= s + len && isspace(*t); t++);
        return(t);
}


static char * skiptowhitespace(char * s)
{
        char * t;

        for (t = s; *t && !isspace(*t); t++);
        return(t);
}

static int line2args(char *line, char **argv, int maxargs)
{
        char *arg;
        int i = 0;

        arg = strtok(line, " \t");
        if ( arg ) {
                argv[i] = arg;
                i++;
        } else
                return 0;

        while( (arg = strtok(NULL, " \t")) && (i <= maxargs)) {
                argv[i] = arg;
                i++;
        }
        return i;
}

/* find a command -- return it if unique otherwise print alternatives */
static command_t *Parser_findargcmd(char *name, command_t cmds[])
{
        command_t *cmd;

        for (cmd = cmds; cmd->pc_name; cmd++) {
                if (strcmp(name, cmd->pc_name) == 0)
                        return cmd;
        }
        return NULL;
}

void Parser_ignore_errors(int ignore)
{
        ignore_errors = ignore;
}

int Parser_execarg(int argc, char **argv, command_t cmds[])
{
        command_t *cmd;

        cmd = Parser_findargcmd(argv[0], cmds);
        if ( cmd ) {
                int rc = (cmd->pc_func)(argc, argv);
                if (rc == CMD_HELP)
                        fprintf(stderr, "%s\n", cmd->pc_help);
                return rc;
        } else {
		printf("Try interactive use without arguments or use one of:\n");
                for (cmd = cmds; cmd->pc_name; cmd++)
                        printf("\"%s\"\n", cmd->pc_name);
                printf("as argument.\n");
        }
        return -1;
}

/* returns the command_t * (NULL if not found) corresponding to a
   _partial_ match with the first token in name.  It sets *next to
   point to the following token. Does not modify *name. */
static command_t * find_cmd(char * name, command_t cmds[], char ** next)
{
        int    i, len;

        if (!cmds || !name )
                return NULL;

        /* This sets name to point to the first non-white space character,
           and next to the first whitespace after name, len to the length: do
           this with strtok*/
        name = skipwhitespace(name);
        *next = skiptowhitespace(name);
        len = (int)(*next - name);
        if (len == 0)
                return NULL;

        for (i = 0; cmds[i].pc_name; i++) {
                if (strncasecmp(name, cmds[i].pc_name, len) == 0) {
                        *next = skipwhitespace(*next);
                        return(&cmds[i]);
                }
        }
        return NULL;
}

/* Recursively process a command line string s and find the command
   corresponding to it. This can be ambiguous, full, incomplete,
   non-existent. */
static int process(char *s, char ** next, command_t *lookup,
                   command_t **result, char **prev)
{
        *result = find_cmd(s, lookup, next);
        *prev = s;

        /* non existent */
        if (!*result)
                return CMD_NONE;

        /* found entry: is it ambigous, i.e. not exact command name and
           more than one command in the list matches.  Note that find_cmd
           points to the first ambiguous entry */
        if (strncasecmp(s, (*result)->pc_name, strlen((*result)->pc_name))) {
                char *another_next;
                command_t *another_result = find_cmd(s, (*result) + 1,
                                                     &another_next);
                int found_another = 0;

                while (another_result) {
                        if (strncasecmp(s, another_result->pc_name,
                                        strlen(another_result->pc_name)) == 0){
                                *result = another_result;
                                *next = another_next;
                                goto got_it;
                        }
                        another_result = find_cmd(s, another_result + 1,
                                                  &another_next);
                        found_another = 1;
                }
                if (found_another)
                        return CMD_AMBIG;
        }

got_it:
        /* found a unique command: component or full? */
        if ( (*result)->pc_func ) {
                return CMD_COMPLETE;
        } else {
                if ( *next == '\0' ) {
                        return CMD_INCOMPLETE;
                } else {
                        return process(*next, next, (*result)->pc_sub_cmd,
                                       result, prev);
                }
        }
}

#ifdef HAVE_LIBREADLINE
static command_t * match_tbl;   /* Command completion against this table */
static char * command_generator(const char * text, int state)
{
        static int index,
                len;
        char       *name;

        /* Do we have a match table? */
        if (!match_tbl)
                return NULL;

        /* If this is the first time called on this word, state is 0 */
        if (!state) {
                index = 0;
                len = (int)strlen(text);
        }

        /* Return next name in the command list that paritally matches test */
        while ( (name = (match_tbl + index)->pc_name) ) {
                index++;

                if (strncasecmp(name, text, len) == 0) {
                        return(strdup(name));
                }
        }

        /* No more matches */
        return NULL;
}

/* probably called by readline */
static char **command_completion(char * text, int start, int end)
{
        command_t   * table;
        char        * pos;

        match_tbl = top_level;
        
        for (table = find_cmd(rl_line_buffer, match_tbl, &pos);
             table; table = find_cmd(pos, match_tbl, &pos)) 
        {

                if (*(pos - 1) == ' ') match_tbl = table->pc_sub_cmd;
        }

        return completion_matches(text, command_generator);
}
#endif

/* take a string and execute the function or print help */
int execute_line(char * line)
{
	command_t	*cmd, *ambig;
	char		*prev;
	char		*next, *tmp;
	char		*argv[MAXARGS];
	int		i;
	int		rc = 0;

	switch (process(line, &next, top_level, &cmd, &prev)) {
	case CMD_AMBIG:
		fprintf(stderr, "Ambiguous command \'%s\'\nOptions: ", line);
		while ((ambig = find_cmd(prev, cmd, &tmp))) {
			fprintf(stderr, "%s ", ambig->pc_name);
			cmd = ambig + 1;
		}
		fprintf(stderr, "\n");
		break;
	case CMD_NONE:
		fprintf(stderr, "No such command, type help\n");
		break;
	case CMD_INCOMPLETE:
		fprintf(stderr, "'%s' incomplete command.  Use '%s x' where "
			"x is one of:\n", line, line);
		fprintf(stderr, "\t");
		for (i = 0; cmd->pc_sub_cmd[i].pc_name; i++)
			fprintf(stderr, "%s ", cmd->pc_sub_cmd[i].pc_name);
		fprintf(stderr, "\n");
		break;
	case CMD_COMPLETE:
		optind = 0;
		i = line2args(line, argv, MAXARGS);
		rc = (cmd->pc_func)(i, argv);

		if (rc == CMD_HELP)
			fprintf(stderr, "%s\n", cmd->pc_help);

		break;
	}

	return rc;
}

int
noop_fn ()
{
        return (0);
}

/* just in case you're ever in an airplane and discover you
   forgot to install readline-dev. :) */
int init_input()
{
        int   interactive = isatty (fileno (stdin));

#ifdef HAVE_LIBREADLINE
        using_history();
        stifle_history(HISTORY);

        if (!interactive)
        {
                rl_prep_term_function = (rl_vintfunc_t *)noop_fn;
                rl_deprep_term_function = (rl_voidfunc_t *)noop_fn;
        }

        rl_attempted_completion_function = (CPPFunction *)command_completion;
        rl_completion_entry_function = (void *)command_generator;
#endif
        return interactive;
}

#ifndef HAVE_LIBREADLINE
#define add_history(s)
char * readline(char * prompt)
{
        int size = 2048;
        char *line = malloc(size);
        char *ptr = line;
        int c;
        int eof = 0;

        if (line == NULL)
                return NULL;
        if (prompt)
                printf ("%s", prompt);

        while (1) {
                if ((c = fgetc(stdin)) != EOF) {
                        if (c == '\n')
                                goto out;
                        *ptr++ = (char)c;

                        if (ptr - line >= size - 1) {
                                char *tmp;

                                size *= 2;
                                tmp = malloc(size);
                                if (tmp == NULL)
                                        goto outfree;
                                memcpy(tmp, line, ptr - line);
                                ptr = tmp + (ptr - line);
                                free(line);
                                line = tmp;
                        }
                } else {
                        eof = 1;
                        if (ferror(stdin) || feof(stdin))
                                goto outfree;
                        goto out;
                }
        }
out:
        *ptr = 0;
        if (eof && (strlen(line) == 0)) {
                free(line);
                line = NULL;
        }
        return line;
outfree:
        free(line);
        return NULL;
}
#endif

/* this is the command execution machine */
int Parser_commands(void)
{
        char *line, *s;
        int rc = 0, save_error = 0;
        int interactive;

        interactive = init_input();

        while(!done) {
                line = readline(interactive ? parser_prompt : NULL);

                if (!line) break;

                s = skipwhitespace(line);

                if (*s) {
                        add_history(s);
                        rc = execute_line(s);
                }
                /* stop on error if not-interactive */
                if (rc != 0 && !interactive) {
                        if (save_error == 0)
                                save_error = rc;
                        if (!ignore_errors)
                                done = 1;
                }

                free(line);
        }
        if (save_error)
                rc = save_error;
        return rc;
}


/* sets the parser prompt */
void Parser_init(char * prompt, command_t * cmds)
{
        done = 0;
        top_level = cmds;
        if (parser_prompt) free(parser_prompt);
        parser_prompt = strdup(prompt);
}

/* frees the parser prompt */
void Parser_exit(int argc, char *argv[])
{
        done = 1;
        free(parser_prompt);
        parser_prompt = NULL;
}

/* convert a string to an integer */
int Parser_int(char *s, int *val)
{
        int ret;

        if (*s != '0')
                ret = sscanf(s, "%d", val);
        else if (*(s+1) != 'x')
                ret = sscanf(s, "%o", val);
        else {
                s++;
                ret = sscanf(++s, "%x", val);
        }

        return(ret);
}


void Parser_qhelp(int argc, char *argv[]) {

        printf("Available commands are:\n");

        print_commands(NULL, top_level);
        printf("For more help type: help command-name\n");
}

int Parser_help(int argc, char **argv)
{
        char line[1024];
        char *next, *prev, *tmp;
        command_t *result, *ambig;
        int i;

        if ( argc == 1 ) {
                Parser_qhelp(argc, argv);
                return 0;
        }

        line[0]='\0';
        for ( i = 1 ;  i < argc ; i++ ) {
		if (strlen(argv[i]) > sizeof(line)-strlen(line)-1)
			return -E2BIG;
		strncat(line, argv[i], sizeof(line)-strlen(line)-1);
        }

        switch ( process(line, &next, top_level, &result, &prev) ) {
        case CMD_COMPLETE:
                fprintf(stderr, "%s: %s\n",line, result->pc_help);
                break;
        case CMD_NONE:
                fprintf(stderr, "%s: Unknown command.\n", line);
                break;
        case CMD_INCOMPLETE:
                fprintf(stderr,
                        "'%s' incomplete command.  Use '%s x' where x is one of:\n",
                        line, line);
                fprintf(stderr, "\t");
                for (i = 0; result->pc_sub_cmd[i].pc_name; i++) {
                        fprintf(stderr, "%s ", result->pc_sub_cmd[i].pc_name);
                }
                fprintf(stderr, "\n");
                break;
        case CMD_AMBIG:
                fprintf(stderr, "Ambiguous command \'%s\'\nOptions: ", line);
                while( (ambig = find_cmd(prev, result, &tmp)) ) {
                        fprintf(stderr, "%s ", ambig->pc_name);
                        result = ambig + 1;
                }
                fprintf(stderr, "\n");
                break;
        }
        return 0;
}


void Parser_printhelp(char *cmd)
{
        char *argv[] = { "help", cmd };
        Parser_help(2, argv);
}


/*************************************************************************
 * COMMANDS                                                              *
 *************************************************************************/
static void print_commands(char * str, command_t * table) {
        command_t * cmds;
        char         buf[80];

        for (cmds = table; cmds->pc_name; cmds++) {
                if (cmds->pc_func) {
                        if (str) printf("\t%s %s\n", str, cmds->pc_name);
                        else printf("\t%s\n", cmds->pc_name);
                }
                if (cmds->pc_sub_cmd) {
                        if (str) {
                                sprintf(buf, "%s %s", str, cmds->pc_name);
                                print_commands(buf, cmds->pc_sub_cmd);
                        } else {
                                print_commands(cmds->pc_name, cmds->pc_sub_cmd);
                        }
                }
        }
}

char *Parser_getstr(const char *prompt, const char *deft, char *res,
                    size_t len)
{
        char *line = NULL;
        int size = strlen(prompt) + strlen(deft) + 8;
        char *theprompt;
        theprompt = malloc(size);
        assert(theprompt);

        sprintf(theprompt, "%s [%s]: ", prompt, deft);

        line  = readline(theprompt);
        free(theprompt);

        if ( line == NULL || *line == '\0' ) {
                strncpy(res, deft, len);
        } else {
                strncpy(res, line, len);
        }

        if ( line ) {
                free(line);
                return res;
        } else {
                return NULL;
        }
}

/* get integer from prompt, loop forever to get it */
int Parser_getint(const char *prompt, long min, long max, long deft, int base)
{
        int rc;
        long result;
        char *line;
        int size = strlen(prompt) + 40;
        char *theprompt = malloc(size);
        assert(theprompt);
        sprintf(theprompt,"%s [%ld, (0x%lx)]: ", prompt, deft, deft);

        fflush(stdout);

        do {
                line = NULL;
                line = readline(theprompt);
                if ( !line ) {
                        fprintf(stdout, "Please enter an integer.\n");
                        fflush(stdout);
                        continue;
                }
                if ( *line == '\0' ) {
                        free(line);
                        result =  deft;
                        break;
                }
                rc = Parser_arg2int(line, &result, base);
                free(line);
                if ( rc != 0 ) {
                        fprintf(stdout, "Invalid string.\n");
                        fflush(stdout);
                } else if ( result > max || result < min ) {
                        fprintf(stdout, "Error: response must lie between %ld and %ld.\n",
                                min, max);
                        fflush(stdout);
                } else {
                        break;
                }
        } while ( 1 ) ;

        if (theprompt)
                free(theprompt);
        return result;

}

/* get boolean (starting with YyNn; loop forever */
int Parser_getbool(const char *prompt, int deft)
{
        int result = 0;
        char *line;
        int size = strlen(prompt) + 8;
        char *theprompt = malloc(size);
        assert(theprompt);

        fflush(stdout);

        if ( deft != 0 && deft != 1 ) {
                fprintf(stderr, "Error: Parser_getbool given bad default %d\n",
                        deft);
                assert ( 0 );
        }
        sprintf(theprompt, "%s [%s]: ", prompt, (deft==0)? "N" : "Y");

        do {
                line = NULL;
                line = readline(theprompt);
                if ( line == NULL ) {
                        result = deft;
                        break;
                }
                if ( *line == '\0' ) {
                        result = deft;
                        break;
                }
                if ( *line == 'y' || *line == 'Y' ) {
                        result = 1;
                        break;
                }
                if ( *line == 'n' || *line == 'N' ) {
                        result = 0;
                        break;
                }
                if ( line )
                        free(line);
                fprintf(stdout, "Invalid string. Must start with yY or nN\n");
                fflush(stdout);
        } while ( 1 );

        if ( line )
                free(line);
        if ( theprompt )
                free(theprompt);
        return result;
}

/* parse int out of a string or prompt for it */
long Parser_intarg(const char *inp, const char *prompt, int deft,
                   int min, int max, int base)
{
        long result;
        int rc;

        rc = Parser_arg2int(inp, &result, base);

        if ( rc == 0 ) {
                return result;
        } else {
                return Parser_getint(prompt, deft, min, max, base);
        }
}

/* parse int out of a string or prompt for it */
char *Parser_strarg(char *inp, const char *prompt, const char *deft,
                    char *answer, int len)
{
        if ( inp == NULL || *inp == '\0' ) {
                return Parser_getstr(prompt, deft, answer, len);
        } else
                return inp;
}

/* change a string into a number: return 0 on success. No invalid characters
   allowed. The processing of base and validity follows strtol(3)*/
int Parser_arg2int(const char *inp, long *result, int base)
{
        char *endptr;

        if ( (base !=0) && (base < 2 || base > 36) )
                return 1;

        *result = strtol(inp, &endptr, base);

        if ( *inp != '\0' && *endptr == '\0' )
                return 0;
        else
                return 1;
}

/* Convert human readable size string to and int; "1k" -> 1000 */
int Parser_size (int *sizep, char *str) {
        int size;
        char mod[32];

        switch (sscanf (str, "%d%1[gGmMkK]", &size, mod)) {
        default:
                return (-1);

        case 1:
                *sizep = size;
                return (0);

        case 2:
                switch (*mod) {
                case 'g':
                case 'G':
                        *sizep = size << 30;
                        return (0);

                case 'm':
                case 'M':
                        *sizep = size << 20;
                        return (0);

                case 'k':
                case 'K':
                        *sizep = size << 10;
                        return (0);

                default:
                        *sizep = size;
                        return (0);
                }
        }
}

/* Convert a string boolean to an int; "enable" -> 1 */
int Parser_bool (int *b, char *str) {
        if (!strcasecmp (str, "no") ||
            !strcasecmp (str, "n") ||
            !strcasecmp (str, "off") ||
            !strcasecmp (str, "down") ||
            !strcasecmp (str, "disable"))
        {
                *b = 0;
                return (0);
        }

        if (!strcasecmp (str, "yes") ||
            !strcasecmp (str, "y") ||
            !strcasecmp (str, "on") ||
            !strcasecmp (str, "up") ||
            !strcasecmp (str, "enable"))
        {
                *b = 1;
                return (0);
        }

        return (-1);
}

int Parser_quit(int argc, char **argv)
{
        argc = argc;
        argv = argv;
        done = 1;
        return 0;
}
