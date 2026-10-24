# myshell - Deliverable 1: basic shell, process management, job control

C++17, Linux. Uses `fork`, `execvp`, `wait4`, `setpgid`, `tcsetpgrp`, `kill`, signals.

## Build & run
```
make                    # builds ./myshell  (g++ >= 7)
./myshell               # interactive shell
./myshell script.msh    # run a script        (-e echoes each line first)
make demo               # run every demo, write evidence/*.log and evidence/SUMMARY.txt
make sanitize           # optional: ASan+UBSan build (./myshell_asan)
```

## Source layout
| file | purpose |
|------|---------|
| `src/main.cpp` | argument handling, picks interactive vs script mode |
| `src/shell.{hpp,cpp}` | read-eval loop, fork/exec/wait, foreground/background, job control, signals, cleanup |
| `src/parser.{hpp,cpp}` | lexer/parser: quotes, `$VAR $? $$ ~`, `; &`, `< > >> 2>`, validation |
| `src/builtins.{hpp,cpp}` | cd pwd exit echo clear ls cat mkdir rmdir rm touch kill + jobs fg bg help |
| `src/job.{hpp,cpp}` | `Job` record (PCB-lite) and `JobTable` - the structure later schedulers reuse |

## The job record (`struct Job`, src/job.hpp)
Identity (`id`, `serial`, `pid`, `pgid`, `command`), state (`New/Ready/Running/Stopped/Done/Terminated`,
`exit_status`, `term_signal`, `stop_signal`), **scheduler fields** (`priority`, `queue_level`,
`burst_estimate_ms`, `remaining_ms`) and **accounting** (`arrival`, `first_run`, `finish`, `cpu_user_ms`,
`cpu_sys_ms`, `stop_count`, derived `turnaround_ms()/response_ms()/wait_ms()`).
Finished jobs move to `JobTable::history()`. `jobs -a` prints every record.

## Syntax
`cmd &` background, `a ; b` sequence, `> >> < 2> 2>>` redirection, `'..'` `".."` `\x` quoting,
`$VAR ${VAR} $? $$ ~`, `# comment`. Pipes, `&&`, `||` are rejected with a clear message (later deliverable).

## Demos (evidence)
`demos/01_builtins.msh` normal use of all built-ins - `02_process_mgmt.msh` fg/bg/stop/continue/kill, process
groups, ps evidence, job records - `03_errors.msh` invalid commands and inputs - `04_adversarial.msh` odd file names,
SIGTERM-ignoring job, stopped-job kill, deleted cwd, bg stdin - `05_nested_and_exit.msh` nested shell, exit guard,
cleanup - `06_limits.msh` (generated) line/arg limits, 100-job burst - `07_interactive_pty.py` real Ctrl-C / Ctrl-Z / fg / bg
through a pseudo-terminal. `run_demos.sh` runs everything and prints PASS/FAIL checks.



## Known limitations (documented in the report)
* No pipes / globbing / `&&`. `rm` has no `-r`. Background-job finish time is stamped when the shell *reaps* the
  job (at the next prompt), not at the instant it exits, so background turnaround is slightly inflated.
* Job control signals (Ctrl-C/Z) need a terminal; scripts use `kill -STOP/-CONT` instead.
