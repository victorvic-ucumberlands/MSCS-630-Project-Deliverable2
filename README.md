# myshell - Deliverables 1-2: shell, process management, job control, CPU scheduling

C++17, Linux. Uses `fork`, `execvp`, `wait4`, `setpgid`, `tcsetpgrp`, `kill`, signals.

## Build & run
```
make                    # builds ./myshell  (g++ >= 7)
./myshell               # interactive shell
./myshell script.msh    # run a script        (-e echoes each line first)
make demo               # run every demo + test, write evidence/*.log and evidence/SUMMARY.txt
make sanitize           # optional: ASan+UBSan build (./myshell_asan)
```

## Source layout
| file | purpose |
|------|---------|
| `src/main.cpp` | argument handling, picks interactive vs script mode |
| `src/shell.{hpp,cpp}` | read-eval loop, fork/exec/wait, foreground/background, job control, signals, cleanup |
| `src/parser.{hpp,cpp}` | lexer/parser: quotes, `$VAR $? $$ ~`, `; &`, `< > >> 2>`, validation |
| `src/builtins.{hpp,cpp}` | cd pwd exit echo clear ls cat mkdir rmdir rm touch kill + jobs fg bg help |
| `src/job.{hpp,cpp}` | `Job` record (PCB-lite) and `JobTable`; also the record of every simulated scheduler task |
| `src/cpu_sched.{hpp,cpp}` | **Deliverable 2** - the resident CPU scheduler every command goes through: FCFS / Round-Robin / Priority (binary heap), dispatch with SIGCONT, preemption with SIGSTOP confirmed by `wait4`, the quantum, block and wake-up detection from `/proc`, statistics |
| `src/scheduler.{hpp,cpp}` | **Deliverable 2** - `sched run` experiments on the same scheduler (other jobs parked), with a measured report and Gantt chart |
| `src/sched_builtin.cpp` | **Deliverable 2** - the `sched` built-in: live / policy / default / ps / trace, and add / list / remove / run / compare / stats |
| `demos/cpuburn.c` | CPU-bound helper used as a scheduler workload; reports how much CPU time it has consumed |

## The job record (`struct Job`, src/job.hpp)
Identity (`id`, `serial`, `pid`, `pgid`, `command`), state (`New/Ready/Running/Stopped/Done/Terminated`,
`exit_status`, `term_signal`, `stop_signal`), **scheduler fields** (`priority`, `queue_level`,
`burst_estimate_ms`, `remaining_ms`) and **accounting** (`arrival`, `first_run`, `finish`, `cpu_user_ms`,
`cpu_sys_ms`, `stop_count`, derived `turnaround_ms()/response_ms()/wait_ms()`).
Finished jobs move to `JobTable::history()`. `jobs -a` prints every record.

## Scheduling (Deliverable 2)

**Every command the shell starts is scheduled by the shell.** It is a real process created stopped; it waits in
a ready queue and only runs when the shell continues it with `SIGCONT`; the shell takes the CPU back with
`SIGSTOP`. One scheduled process runs at a time. Commands typed normally get the defaults:
**Round-Robin, 100 ms time slice, priority 10, block detection on.**

```
cpuburn A 300 100 &            # ordinary commands: scheduled with the defaults
cpuburn B 300 100 &
sched ps                       # one RUN, the other READY (stopped in the kernel: T)
sched trace on                 # print every scheduling decision as it happens
sched policy priority          # change the policy for everything (rr [-q MS] | priority [--no-preempt] | fcfs)
prio 1 cpuburn urgent 100 &    # one command with priority 1 (lower = more urgent): preempts at once
prio 2 %1                      # change a running job's priority
sched default 5                # priority for new commands
sched live off                 # let the kernel schedule the jobs instead (Deliverable 1 behaviour)
```
`kill`, `bg`, `fg`, Ctrl-C and Ctrl-Z go through the scheduler. The foreground job owns the terminal while it is
not stopped; while it waits in the ready queue the shell holds the terminal and applies Ctrl-C / Ctrl-Z itself.
A process that blocks (sleep, keyboard input, disk) gives the CPU away and rejoins the queue when it wakes.
`jobs -a` shows turnaround, waiting (time in the ready queue), response, CPU held, time blocked for every job.

Experiments: queue a workload with arrival times and priorities, run it under one policy, get a measured report.
The shell's other jobs are parked for the duration.
```
sched add demos/cpuburn A 300 100           # -p priority, -t arrival time (ms), -o output file
sched add -p 1 -t 300 demos/cpuburn B 100 50
sched run rr -q 100 -v                      # event log, kernel states, Gantt chart, per-process metrics
sched run priority [--no-preempt]
sched compare -q 25,100                     # the same workload under every policy
```
Options: `-c MS` context-switch cost, `-T MS` time budget, `-v` verbose, `--quiet`, `--no-block-detect`.

`tests/verify_schedule.py ./myshell ./demos/cpuburn` checks 30 scheduler properties on real runs, including
mutual exclusion sampled independently from `/proc`, for experiments and for ordinary commands.

## Syntax
`cmd &` background, `a ; b` sequence, `> >> < 2> 2>>` redirection, `'..'` `".."` `\x` quoting,
`$VAR ${VAR} $? $$ ~`, `# comment`. Pipes, `&&`, `||` are rejected with a clear message (later deliverable).

## Demos (evidence)
`demos/01_builtins.msh` normal use of all built-ins - `02_process_mgmt.msh` fg/bg/stop/continue/kill, process
groups, ps evidence, job records - `03_errors.msh` invalid commands and inputs - `04_adversarial.msh` odd file names,
SIGTERM-ignoring job, stopped-job kill, deleted cwd, bg stdin - `05_nested_and_exit.msh` nested shell, exit guard,
cleanup - `06_limits.msh` (generated) line/arg limits, 100-job burst - `07_interactive_pty.py` real Ctrl-C / Ctrl-Z / fg / bg
through a pseudo-terminal. **Deliverable 2:** `15_live_sched.msh` (ordinary commands scheduled with defaults, trace, block
detection, `prio`, renice, job control through the scheduler, live off/on), `16_live_pty.py` (Ctrl-Z / Ctrl-C on a
queued foreground job, interactive `cat` beside a CPU hog), `08_roundrobin.msh`, `09_priority.msh`,
`10_sched_errors.msh`, `11_sched_limits.msh` (generated, 64 real processes), `12_sched_pty.py`, `13_performance.py`
(experiments E1-E7). `run_demos.sh` runs everything and prints PASS/FAIL checks.

For screenshots: run `./myshell` yourself in a terminal and type the commands from the demo scripts
(or `./myshell -e demos/02_process_mgmt.msh` for a ready-made transcript).

## Known limitations (documented in the reports)
* No pipes / globbing / `&&`. `rm` has no `-r`. A Deliverable 1 background job's finish time is stamped when the
  shell *reaps* it (at the next prompt), not at the instant it exits.
* Job control signals (Ctrl-C/Z) need a terminal; scripts use `kill -STOP/-CONT` instead.
* Blocking is detected by sampling `/proc` every 5 ms: a process that wakes up can run beside the dispatched one
  until the next probe (at most ~5 ms).
* Built-ins run inside the shell and are not scheduled; a background `sched run ... &` is a separate scheduler.
