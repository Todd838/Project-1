# Quash: A Small UNIX Command Shell

**Authors:** Todd Perkins
**Course:** Fall 2026 Operating Systems
**Files:** `shell.c` (all source), `README.md` (this report)

---

## 1. Overview

Quash is a command-line shell written in C. It reads a line of input, interprets built-in commands itself, and runs every other command as a separate process. It is a simplified version of shells such as `sh`, `bash`, `csh`, and `tcsh`.

The project covers four areas of systems programming:

- Process creation with `fork`, program loading with `execvp`, and synchronization with `waitpid`.
- Signal handling so that Ctrl-C affects the running job, not the shell.
- Timed termination of a foreground job using `alarm`, `SIGALRM`, and `kill`.
- Standard I/O redirection and pipes with `open`, `dup2`, and `pipe`.

Every feature from Tasks 1 through 6 is implemented, including the extra-credit redirection and pipe features.

## 2. Building and Running

```
gcc -Wall -Wextra -o quash shell.c
./quash
```

The shell has no dependencies beyond the C standard library and POSIX. It compiles without warnings under `-Wall -Wextra`. Type `exit` or press Ctrl-D to quit.

## 3. Program Structure

All code lives in `shell.c`. The file is organized in this order:

| Section | Functions | Purpose |
|---|---|---|
| Globals and types | `delimiters`, `environ`, `Command` | Shared constants and the per-stage command record |
| Signal handling | `on_alarm`, `on_sigint`, `install_handler` | Install handlers for SIGINT and SIGALRM |
| Input and parsing | `print_prompt`, `expand`, `parse`, `free_commands` | Build `Command` records from a line |
| Built-ins | `builtin_cd`, `builtin_pwd`, `builtin_echo`, `builtin_env`, `builtin_setenv`, `run_builtin` | Commands executed inside the shell |
| External commands | `exec_stage`, `run_pipeline`, `wait_foreground`, `close_pipes`, `reap_background` | Fork, connect, exec, and wait |
| Entry point | `main` | Read-eval loop |

### The `Command` record

Each stage of a command line is stored in one `Command` structure:

```c
typedef struct {
    char *argv[MAX_COMMAND_LINE_ARGS]; // argv[0] is the program, NULL-terminated
    int argc;
    char *infile;   // target of "<", or NULL
    char *outfile;  // target of ">", or NULL
} Command;
```

A command line such as `cat in.txt | sort > out.txt` becomes an array of two `Command` values. The first has `argv = {"cat", "in.txt"}`, and the second has `argv = {"sort"}` with `outfile = "out.txt"`. Storing redirections on the stage they belong to makes the pipeline code simple: every stage is handled the same way, and only the first and last stages differ in what they read and write.

## 4. Design Decisions

### 4.1 Tokenizing and variable expansion

`parse` splits the line on the delimiter set `" \t\r\n"` using `strtok_r`. The reentrant form is used so that the function's state stays local. Each token is then classified:

- `|` starts a new `Command`.
- `<` and `>` consume the next token as a file name.
- A token ending in `&` as the last word marks the line as a background job. The `&` is removed before the rest of the parsing runs.
- Any other token is appended to the current stage's `argv`.

Every argument passes through `expand`. A token that begins with `$` is replaced by the value of that environment variable, or by an empty string if the variable is unset. The result is always a freshly allocated copy. This was a deliberate choice: `getenv` returns a pointer into the environment, and `setenv` can invalidate that pointer. For example, `setenv x=$x` would otherwise read from memory that `setenv` is in the middle of changing. Copying every argument avoids this problem and also gives every command a uniform ownership rule: `free_commands` frees all argument strings after execution.

Expansion happens during parsing, so `$VAR` works in every built-in and every external command, as the spec requires.

### 4.2 Built-in commands run in the parent

`cd`, `setenv`, and `exit` must change the state of the shell process itself, so they have to run in the parent, not in a child. The rule in `main` is: a single stage whose program name is a built-in runs in-process; anything else goes through `run_pipeline`.

Because built-ins run in-process, redirection needs a different approach. `run_builtin` saves copies of file descriptors 0 and 1 with `dup`, applies any `<` or `>` with `dup2`, runs the built-in, flushes `stdout`, and then restores the saved descriptors. Output is therefore redirected correctly even for `echo`, and the shell's own stdin and stdout are never left changed.

Built-ins do not take part in pipelines. In a pipeline, a built-in name is looked up as an external program. This is a limitation, not an oversight: a built-in such as `cd` in the middle of a pipeline would run in a child process and could not affect the shell.

### 4.3 External commands: fork, exec, wait

For an external command, `run_pipeline` creates one child per stage. Each child calls `exec_stage`, which connects its stdin and stdout to the correct pipe ends, closes all pipe descriptors it no longer needs, applies any file redirection, and then calls `execvp`. `execvp` searches `PATH`, so commands such as `ls` and `cat` work without a full path.

If `execvp` returns, the call failed. The child prints the required two-line error message and calls `_exit(127)`. The child uses `_exit` rather than `exit` so that it does not flush the parent's buffered stdio data a second time.

The parent closes every pipe descriptor, which is required for correctness: if the parent kept the write end of a pipe open, readers such as `sort` would never see end-of-file. It then either waits for the children or, for a background job, returns immediately.

### 4.4 Signal handling

The shell must survive Ctrl-C but the foreground job must not. The design uses two sides of the same mechanism:

- **In the shell:** `SIGINT` is handled by `on_sigint`, which writes a newline. The handler is installed with `sigaction` and `sa_flags = 0`, which deliberately omits `SA_RESTART`. Without that flag, a Ctrl-C arriving during `fgets` or `waitpid` would make the call restart silently. With it, the call returns `EINTR`, which the shell handles.
- **In children:** `exec_stage` explicitly resets `SIGINT` and `SIGALRM` to `SIG_DFL` before `execvp`. Caught signals already revert to their default action across `exec`, so this is a safety measure that makes the intent explicit. It also guarantees that the program dies on Ctrl-C the way a normal program does.

When the terminal sends `SIGINT` to the foreground process group, the child dies and the shell's `waitpid` returns normally. The shell then prints the prompt again. If the `SIGINT` interrupts `fgets` at the prompt, `main` calls `clearerr(stdin)` and continues. This clears the error flag that `fgets` sets on interruption, so the shell does not mistake it for end-of-file.

### 4.5 The 10-second foreground timer

The timer is implemented with `alarm`. Before waiting for a foreground command line, `wait_foreground` resets a flag and calls `alarm(10)`. The `SIGALRM` handler sets the flag. Because the handler was installed without `SA_RESTART`, the blocked `waitpid` returns `EINTR` when the alarm fires. The loop then checks the flag, prints a message, and sends `SIGKILL` to every process in the command line that is still running. The loop continues waiting so that the killed processes are reaped and do not become zombies. Finally, `alarm(0)` cancels any pending alarm.

Two details matter here:

- Only foreground command lines are timed. Background jobs are not started with the alarm, so they are never killed by it.
- `pids[i]` is set to `-1` after each child is reaped, so the kill loop never sends a signal to a process ID that has already been released and might have been reused.

`SIGKILL` is used rather than `SIGTERM` because the spec asks for termination, and a process that ignores `SIGTERM` should still be stopped.

### 4.6 Background jobs and zombie cleanup

A trailing `&` causes `run_pipeline` to skip `wait_foreground`. The shell returns to the prompt at once. Finished background children are collected by `reap_background`, which calls `waitpid(-1, NULL, WNOHANG)` in a loop before each prompt. This prevents zombie processes without blocking the shell. The function is safe to call at that point because all foreground children have already been reaped synchronously.

### 4.7 Redirection and pipes

`<` and `>` are stored on the `Command` stage in which they appear. In `exec_stage`, a `<` file is opened read-only and duplicated onto fd 0, and a `>` file is opened with `O_WRONLY | O_CREAT | O_TRUNC` (mode `0644`) and duplicated onto fd 1. A redirection always overrides the pipe on the same side. For example, in `cat x | sort > y`, the output goes to `y`, not to the pipe.

Pipelines are built with an array of `int[2]` pipes, one per boundary between stages. Stage `i` reads from `pipes[i-1][0]` and writes to `pipes[i][1]`. The number of stages is capped at `MAX_PIPE_STAGES` (16). If `pipe` fails partway through creation, the pipes already made are closed before returning.

## 5. Limitations and Known Issues

- **No job control.** `fg`, `bg`, `jobs`, and Ctrl-Z are not implemented. Background jobs are simply not waited on.
- **Built-ins in pipelines** run as external programs and are not interpreted (see 4.2).
- **`cd -`** and other shell-specific `cd` forms are not supported.
- **Variable syntax** is limited to `$NAME`. There is no `${NAME}`, quoting, or globbing, and the delimiter set is fixed to whitespace.
- **Operators must be separated by spaces.** `echo hi>out.txt` is parsed as the word `hi>out.txt`.
- **Limits** are fixed at compile time: 1024 characters per line, 128 arguments per command, and 16 pipeline stages.
- **Timer scope.** The 10-second limit applies to the whole foreground command line, not to each stage individually.

## 6. Testing

Testing was done in two ways.

**Scripted input.** The shell was run with piped input to check the basic behavior: `echo` with `$` variables, `setenv` in both forms, `env` with and without arguments, `cd` followed by `pwd`, a missing command that produces the required error, `>` and `<` redirection, a two-stage pipeline (`echo a b | tr a-z A-Z`), a background job, and `exit`. All produced the expected output and exit status.

**Interactive testing.** Ctrl-C at the prompt and during a running foreground job, and the 10-second timeout on a long-running command, depend on terminal signal delivery and have not yet been verified by hand. These should be checked in a real terminal before submission (for example, running `sleep 30` and pressing Ctrl-C, then `sleep 30` and waiting for the timeout).

## 7. Summary

Quash implements every required feature and both extra-credit redirection features. Its design keeps the code straightforward. Built-ins run in the shell itself, every external command uses one fork-and-exec path, and each stage of a pipeline is described by the same `Command` record. Signal behavior is controlled by installing handlers without `SA_RESTART` and by resetting signals in children, so the shell survives Ctrl-C and long-running jobs are stopped without affecting the shell.
