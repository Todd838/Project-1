// shell.c - Quash, a small UNIX command shell
// Authors: Todd Perkins
//
// Features:
//   - Prompt showing the current working directory ("/path/to/dir> ")
//   - Built-ins: cd, pwd, echo, exit, env, setenv
//   - $VAR expansion for every token (echo, cd, setenv, ...)
//   - External commands via fork/execvp, with the shell waiting for them
//   - Background jobs with a trailing '&'
//   - Ctrl-C interrupts the foreground job but not the shell
//   - Foreground jobs that run longer than 10 seconds are killed
//   - Extra credit: output redirection (>), input redirection (<), and pipes (|)

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <assert.h>

#define MAX_COMMAND_LINE_LEN 1024
#define MAX_COMMAND_LINE_ARGS 128
#define MAX_PIPE_STAGES 16
#define TIME_LIMIT_SECONDS 10

char delimiters[] = " \t\r\n";
extern char **environ;

// One stage of a (possibly piped) command line.
typedef struct {
    char *argv[MAX_COMMAND_LINE_ARGS]; // argv[0] is the command, NULL-terminated
    int argc;
    char *infile;   // target of "<", or NULL
    char *outfile; // target of ">", or NULL
} Command;

static volatile sig_atomic_t alarm_fired = 0;

// SIGALRM: the foreground time limit expired.
static void on_alarm(int sig) {
    (void)sig;
    alarm_fired = 1;
}

// SIGINT in the shell: just move to a fresh line so the shell survives Ctrl-C.
// (The foreground child gets the default SIGINT action and dies instead.)
static void on_sigint(int sig) {
    (void)sig;
    write(STDOUT_FILENO, "\n", 1);
}

// Install a handler WITHOUT SA_RESTART so blocking calls (fgets, waitpid)
// return EINTR when a signal arrives, which we need for the timer and Ctrl-C.
static void install_handler(int sig, void (*fn)(int)) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fn;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(sig, &sa, NULL) < 0) {
        perror("sigaction");
        exit(1);
    }
}

// Print "<cwd>> " as the prompt.
static void print_prompt(void) {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof cwd) == NULL) {
        perror("getcwd");
        strcpy(cwd, "?");
    }
    printf("%s> ", cwd);
    fflush(stdout);
}

// Return a heap copy of tok. A token of the form $NAME becomes the value of
// the environment variable NAME (empty string if unset). We copy the value so
// that later setenv() calls cannot invalidate our argument strings.
static char *expand(const char *tok) {
    if (tok[0] == '$' && tok[1] != '\0') {
        const char *val = getenv(tok + 1);
        return strdup(val ? val : "");
    }
    return strdup(tok);
}

// Split line into pipeline stages and redirections. Returns 0 on success,
// -1 on a syntax error. *ncmds is always valid on return so the caller can
// free whatever was allocated. Modifies line (strtok_r).
static int parse(char *line, Command cmds[], int *ncmds, bool *background) {
    char *tokens[MAX_COMMAND_LINE_ARGS];
    int ntok = 0;
    char *save = NULL;
    for (char *t = strtok_r(line, delimiters, &save);
         t != NULL && ntok < MAX_COMMAND_LINE_ARGS - 1;
         t = strtok_r(NULL, delimiters, &save)) {
        tokens[ntok++] = t;
    }

    memset(&cmds[0], 0, sizeof cmds[0]);
    *ncmds = 1;
    *background = false;

    // A trailing '&' (alone, or glued to the last word) means background.
    if (ntok > 0) {
        char *last = tokens[ntok - 1];
        size_t len = strlen(last);
        if (len > 0 && last[len - 1] == '&') {
            *background = true;
            last[len - 1] = '\0';
            if (last[0] == '\0') ntok--;
        }
    }

    if (ntok == 0) {
        *ncmds = 0;   // blank line
        return 0;
    }

    for (int i = 0; i < ntok; i++) {
        char *t = tokens[i];
        Command *cur = &cmds[*ncmds - 1];

        if (strcmp(t, "|") == 0) {
            if (cur->argc == 0) {
                fprintf(stderr, "syntax error near '|'\n");
                return -1;
            }
            if (*ncmds == MAX_PIPE_STAGES) {
                fprintf(stderr, "too many pipeline stages (max %d)\n", MAX_PIPE_STAGES);
                return -1;
            }
            memset(&cmds[*ncmds], 0, sizeof cmds[0]);
            (*ncmds)++;
        } else if (strcmp(t, "<") == 0 || strcmp(t, ">") == 0) {
            if (i + 1 >= ntok) {
                fprintf(stderr, "syntax error: missing file after '%s'\n", t);
                return -1;
            }
            char **slot = (t[0] == '<') ? &cur->infile : &cur->outfile;
            free(*slot);
            *slot = expand(tokens[++i]);
        } else if (cur->argc < MAX_COMMAND_LINE_ARGS - 1) {
            cur->argv[cur->argc++] = expand(t);
        }
    }

    for (int k = 0; k < *ncmds; k++) {
        if (cmds[k].argc == 0) {
            fprintf(stderr, "syntax error: empty command in pipeline\n");
            return -1;
        }
        cmds[k].argv[cmds[k].argc] = NULL;
    }
    return 0;
}

static void free_commands(Command cmds[], int ncmds) {
    for (int k = 0; k < ncmds; k++) {
        for (int i = 0; i < cmds[k].argc; i++) free(cmds[k].argv[i]);
        free(cmds[k].infile);
        free(cmds[k].outfile);
    }
}

// ---------- Built-in commands ----------

static bool is_builtin(const char *name) {
    return strcmp(name, "cd") == 0 || strcmp(name, "pwd") == 0 ||
           strcmp(name, "echo") == 0 || strcmp(name, "exit") == 0 ||
           strcmp(name, "env") == 0 || strcmp(name, "setenv") == 0;
}

static void builtin_cd(const Command *c) {
    const char *dir = (c->argc > 1) ? c->argv[1] : getenv("HOME");
    if (dir == NULL) {
        fprintf(stderr, "cd: HOME is not set\n");
        return;
    }
    if (chdir(dir) < 0) perror("cd");
}

static void builtin_pwd(void) {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof cwd) == NULL) perror("pwd");
    else printf("%s\n", cwd);
}

static void builtin_echo(const Command *c) {
    for (int i = 1; i < c->argc; i++) {
        if (i > 1) putchar(' ');
        fputs(c->argv[i], stdout);
    }
    putchar('\n');
}

// With no arguments, print every NAME=value pair. With arguments, print the
// value of each named variable.
static void builtin_env(const Command *c) {
    if (c->argc == 1) {
        for (char **e = environ; *e != NULL; e++) printf("%s\n", *e);
        return;
    }
    for (int i = 1; i < c->argc; i++) {
        const char *v = getenv(c->argv[i]);
        if (v != NULL) printf("%s\n", v);
    }
}

// Accepts "setenv NAME VALUE" or "setenv NAME=VALUE".
static void builtin_setenv(const Command *c) {
    const char *name;
    const char *value;
    char *eq;

    if (c->argc == 3) {
        name = c->argv[1];
        value = c->argv[2];
    } else if (c->argc == 2 && (eq = strchr(c->argv[1], '=')) != NULL) {
        *eq = '\0';
        name = c->argv[1];
        value = eq + 1;
    } else if (c->argc == 2) {
        name = c->argv[1];
        value = "";
    } else {
        fprintf(stderr, "usage: setenv NAME VALUE  or  setenv NAME=VALUE\n");
        return;
    }
    if (setenv(name, value, 1) < 0) perror("setenv");
}

// Open the redirection targets in the shell itself (for built-ins).
static bool redirect_fds(const Command *c) {
    if (c->infile != NULL) {
        int fd = open(c->infile, O_RDONLY);
        if (fd < 0) {
            perror(c->infile);
            return false;
        }
        dup2(fd, STDIN_FILENO);
        close(fd);
    }
    if (c->outfile != NULL) {
        int fd = open(c->outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            perror(c->outfile);
            return false;
        }
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }
    return true;
}

// Run a built-in in the shell process, temporarily applying redirections and
// restoring the original stdin/stdout afterward.
static void run_builtin(const Command *c) {
    int saved_in = dup(STDIN_FILENO);
    int saved_out = dup(STDOUT_FILENO);

    if (redirect_fds(c)) {
        const char *name = c->argv[0];
        if (strcmp(name, "cd") == 0) builtin_cd(c);
        else if (strcmp(name, "pwd") == 0) builtin_pwd();
        else if (strcmp(name, "echo") == 0) builtin_echo(c);
        else if (strcmp(name, "env") == 0) builtin_env(c);
        else if (strcmp(name, "setenv") == 0) builtin_setenv(c);
        else if (strcmp(name, "exit") == 0) {
            fflush(stdout);
            exit(0);
        }
    }

    fflush(stdout);
    dup2(saved_in, STDIN_FILENO);
    dup2(saved_out, STDOUT_FILENO);
    close(saved_in);
    close(saved_out);
}

// ---------- External commands and pipelines ----------

static void close_pipes(int (*pipes)[2], int count) {
    for (int k = 0; k < count; k++) {
        close(pipes[k][0]);
        close(pipes[k][1]);
    }
}

// Runs in the child process. Never returns.
static void exec_stage(Command cmds[], int n, int i, int (*pipes)[2]) {
    // Children must get the default Ctrl-C / timer behavior.
    signal(SIGINT, SIG_DFL);
    signal(SIGALRM, SIG_DFL);

    if (i > 0) dup2(pipes[i - 1][0], STDIN_FILENO);
    if (i < n - 1) dup2(pipes[i][1], STDOUT_FILENO);
    close_pipes(pipes, n - 1);

    Command *c = &cmds[i];
    if (c->infile != NULL) {
        int fd = open(c->infile, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "%s: %s\n", c->infile, strerror(errno));
            _exit(1);
        }
        dup2(fd, STDIN_FILENO);
        close(fd);
    }
    if (c->outfile != NULL) {
        int fd = open(c->outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            fprintf(stderr, "%s: %s\n", c->outfile, strerror(errno));
            _exit(1);
        }
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }

    execvp(c->argv[0], c->argv);
    fprintf(stderr, "execvp() failed: %s\n", strerror(errno));
    fprintf(stderr, "An error occurred.\n");
    _exit(127);
}

// Wait for the foreground processes, enforcing the time limit. If the timer
// fires, kill everything still running in this command line.
static void wait_foreground(pid_t pids[], int n) {
    bool killed = false;

    alarm_fired = 0;
    alarm(TIME_LIMIT_SECONDS);

    for (int i = 0; i < n; i++) {
        if (pids[i] <= 0) continue;
        int status;
        while (true) {
            pid_t r = waitpid(pids[i], &status, 0);
            if (r >= 0) break;
            if (errno != EINTR) {
                perror("waitpid");
                break;
            }
            // Interrupted by SIGINT (shell ignores it) or by SIGALRM.
            if (alarm_fired && !killed) {
                fprintf(stderr, "Process exceeded %d seconds and was terminated.\n",
                        TIME_LIMIT_SECONDS);
                for (int k = 0; k < n; k++) {
                    if (pids[k] > 0) kill(pids[k], SIGKILL);
                }
                killed = true;
            }
        }
        pids[i] = -1;   // reaped
    }

    alarm(0);
}

// Start every stage of a command line, connected by pipes. Foreground command
// lines are waited for; background ones are left running.
static void run_pipeline(Command cmds[], int n, bool background) {
    int pipes[MAX_PIPE_STAGES][2];
    pid_t pids[MAX_PIPE_STAGES];

    for (int i = 0; i < n - 1; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("pipe");
            close_pipes(pipes, i);
            return;
        }
    }

    fflush(stdout);
    for (int i = 0; i < n; i++) {
        pids[i] = fork();
        if (pids[i] < 0) {
            perror("fork");
            pids[i] = -1;
        } else if (pids[i] == 0) {
            exec_stage(cmds, n, i, pipes);
        }
    }

    close_pipes(pipes, n - 1);

    if (!background) {
        wait_foreground(pids, n);
    }
}

// Reap finished background children without blocking.
static void reap_background(void) {
    while (waitpid(-1, NULL, WNOHANG) > 0) {
        // nothing to do; just collect the exit status
    }
}

int main(void) {
    char command_line[MAX_COMMAND_LINE_LEN];

    install_handler(SIGINT, on_sigint);
    install_handler(SIGALRM, on_alarm);

    while (true) {
        reap_background();
        print_prompt();

        if (fgets(command_line, MAX_COMMAND_LINE_LEN, stdin) == NULL) {
            if (feof(stdin)) {          // Ctrl-D
                printf("\n");
                fflush(stdout);
                return 0;
            }
            clearerr(stdin);            // interrupted (Ctrl-C): just re-prompt
            continue;
        }

        size_t len = strlen(command_line);
        if (len > 0 && command_line[len - 1] == '\n') {
            command_line[--len] = '\0';
        } else {
            // Line longer than the buffer: discard the remainder.
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF) { }
        }

        Command cmds[MAX_PIPE_STAGES];
        int ncmds = 0;
        bool background = false;

        if (parse(command_line, cmds, &ncmds, &background) == 0 && ncmds > 0) {
            if (ncmds == 1 && is_builtin(cmds[0].argv[0])) {
                run_builtin(&cmds[0]);
            } else {
                run_pipeline(cmds, ncmds, background);
            }
        }
        free_commands(cmds, ncmds);
    }

    // This should never be reached.
    return -1;
}
