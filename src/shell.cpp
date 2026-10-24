// shell.cpp - read-eval loop, process creation (fork/exec/wait) and job control.
//
// Process model
// -------------
//  * Every external command runs in its own child created with fork() and
//    replaced with execvp().  The child is placed in its *own process group*
//    (pgid == pid) so signals aimed at a job (Ctrl-C, Ctrl-Z, kill %1) never
//    hit the shell or other jobs.
//  * Foreground job : the shell gives it the terminal (tcsetpgrp) and blocks in
//    wait4(pid, WUNTRACED) until it exits, is killed, or is stopped.
//  * Background job : the shell prints "[id] pid" and carries on.  Its state is
//    collected later, without blocking, by reap_children() (wait4 + WNOHANG)
//    which runs before every prompt - so no zombies accumulate.
//  * Built-ins that change shell state (cd, exit, fg, bg, jobs, kill) run in
//    the shell process itself; if the user appends '&' to a built-in it is run
//    in a forked child like any other command.
#include "shell.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "builtins.hpp"

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// Open the target of a redirection.  Prints a diagnostic and returns -1 on error.
static int open_redirect(const Redirect& r) {
  int flags = 0;
  switch (r.kind) {
    case Redirect::Kind::In:     flags = O_RDONLY; break;
    case Redirect::Kind::Out:    flags = O_WRONLY | O_CREAT | O_TRUNC; break;
    case Redirect::Kind::Append: flags = O_WRONLY | O_CREAT | O_APPEND; break;
  }
  int fd = open(r.path.c_str(), flags, 0644);
  if (fd < 0) fprintf(stderr, "myshell: %s: %s\n", r.path.c_str(), strerror(errno));
  return fd;
}

static long tv_ms(const struct timeval& tv) { return tv.tv_sec * 1000L + tv.tv_usec / 1000L; }

// ---------------------------------------------------------------------------
// Construction: put the shell in its own process group and take the terminal.
// ---------------------------------------------------------------------------
Shell::Shell(bool interactive, bool echo_input)
    : interactive_(interactive), echo_input_(echo_input) {
  shell_pid = getpid();
  shell_pgid_ = getpgrp();
  signal(SIGPIPE, SIG_IGN);  // a closed pipe must not kill the shell; write() gets EPIPE instead

  // A fresh shell has no "previous directory": drop an inherited OLDPWD so
  // `cd -` cannot jump to a directory belonging to the parent shell.
  unsetenv("OLDPWD");
  char cwd[4096];
  if (getcwd(cwd, sizeof cwd)) setenv("PWD", cwd, 1);

  if (!interactive_) return;  // scripts: no terminal, no job-control signals

  tty_fd_ = STDIN_FILENO;
  // If we were started in the background, wait until we own the terminal.
  while (true) {
    pid_t fg = tcgetpgrp(tty_fd_);
    if (fg < 0 || fg == (shell_pgid_ = getpgrp())) break;
    kill(-shell_pgid_, SIGTTIN);
  }
  // The shell itself must survive Ctrl-C / Ctrl-Z / Ctrl-\ typed at the prompt,
  // and must be allowed to call tcsetpgrp() from a non-foreground group.
  signal(SIGINT, SIG_IGN);
  signal(SIGQUIT, SIG_IGN);
  signal(SIGTSTP, SIG_IGN);
  signal(SIGTTIN, SIG_IGN);
  signal(SIGTTOU, SIG_IGN);

  if (setpgid(shell_pid, shell_pid) == 0) shell_pgid_ = shell_pid;  // EPERM if session leader: fine
  else shell_pgid_ = getpgrp();
  tcsetpgrp(tty_fd_, shell_pgid_);
  tcgetattr(tty_fd_, &shell_tmodes_);  // remember sane terminal settings
}

void Shell::give_terminal_to(pid_t pgid) {
  if (interactive_) tcsetpgrp(tty_fd_, pgid);
}

std::string Shell::lookup_var(const std::string& name) {
  if (name == "?") return std::to_string(last_status);
  if (name == "$") return std::to_string(shell_pid);
  const char* v = getenv(name.c_str());
  return v ? v : "";
}

std::string Shell::prompt() {
  char buf[4096];
  std::string cwd = getcwd(buf, sizeof buf) ? buf : "?";
  const char* home = getenv("HOME");
  if (home && *home && cwd.compare(0, strlen(home), home) == 0 &&
      (cwd.size() == strlen(home) || cwd[strlen(home)] == '/'))
    cwd = "~" + cwd.substr(strlen(home));
  return "myshell:" + cwd + "$ ";
}

// ---------------------------------------------------------------------------
// Read-eval loop
// ---------------------------------------------------------------------------
int Shell::run(FILE* in) {
  char* buf = nullptr;
  size_t cap = 0;

  while (!exit_requested) {
    reap_children(true);  // report finished/stopped background jobs before prompting
    if (interactive_) {
      std::string p = prompt();
      fputs(p.c_str(), stdout);
      fflush(stdout);
    }

    ssize_t n = getline(&buf, &cap, in);
    if (n < 0) {
      if (errno == EINTR) { clearerr(in); continue; }
      if (interactive_) fputs("exit\n", stdout);  // Ctrl-D
      exit_code = last_status;
      break;
    }
    std::string line(buf, (size_t)n);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();

    if (echo_input_ && !interactive_) {
      printf("myshell$ %s\n", line.c_str());
      fflush(stdout);
    }
    execute_line(line);
  }
  free(buf);
  fflush(stdout);
  cleanup_jobs();
  return exit_code;
}

// Validate the whole line first (syntax errors run nothing), then execute
// command by command so that $? and cd take effect for later commands.
void Shell::execute_line(const std::string& line) {
  if (line.size() > Parser::kMaxLine) {
    fprintf(stderr, "myshell: input line too long (%zu bytes, limit %zu)\n", line.size(),
            Parser::kMaxLine);
    last_status = 2;
    return;
  }
  if (line.find('\0') != std::string::npos) {
    fprintf(stderr, "myshell: invalid input (embedded NUL byte)\n");
    last_status = 2;
    return;
  }

  Command cmd;
  {  // pass 1: syntax check with a dummy variable lookup
    Parser check(line, [](const std::string&) { return std::string("x"); });
    for (;;) {
      Parser::Status st = check.next(cmd);
      if (st == Parser::Status::End) break;
      if (st == Parser::Status::Error) {
        fprintf(stderr, "myshell: %s\n", check.error().c_str());
        last_status = 2;
        return;
      }
    }
  }
  // pass 2: real expansion + execution
  Parser p(line, [this](const std::string& n) { return lookup_var(n); });
  while (!exit_requested) {
    Parser::Status st = p.next(cmd);
    if (st == Parser::Status::End) break;
    if (st == Parser::Status::Error) {
      fprintf(stderr, "myshell: %s\n", p.error().c_str());
      last_status = 2;
      break;
    }
    run_command(cmd);
  }
}

void Shell::run_command(const Command& cmd) {
  if (cmd.argv.empty()) return;
  if (cmd.argv[0] != "exit") exit_warned = false;

  BuiltinFn fn = find_builtin(cmd.argv[0]);
  if (fn && !cmd.background)
    run_builtin_inline(fn, cmd);
  else
    launch(cmd, fn);  // external command, or built-in with '&'
}

// Run a built-in inside the shell process, applying redirections temporarily:
// the original descriptors are saved with dup() and restored afterwards.
void Shell::run_builtin_inline(BuiltinFn fn, const Command& cmd) {
  fflush(stdout);
  fflush(stderr);
  int saved[3] = {-1, -1, -1};
  bool ok = true;

  for (const Redirect& r : cmd.redirs) {
    int target = open_redirect(r);
    if (target < 0) { ok = false; break; }
    if (saved[r.fd] < 0) saved[r.fd] = dup(r.fd);
    dup2(target, r.fd);
    close(target);
  }
  int status = ok ? fn(*this, cmd.argv) : 1;

  fflush(stdout);
  fflush(stderr);
  for (int fd = 0; fd < 3; ++fd)
    if (saved[fd] >= 0) { dup2(saved[fd], fd); close(saved[fd]); }
  last_status = status;
}

// ---------------------------------------------------------------------------
// Process creation
// ---------------------------------------------------------------------------

// Executed in the child after fork(): never returns.
void Shell::child_exec(const Command& cmd, BuiltinFn fn) {
  // 1. New process group; foreground jobs also take the terminal.  Both parent
  //    and child call setpgid() to close the race of who runs first.
  setpgid(0, 0);
  if (!cmd.background) give_terminal_to(getpid());

  // 2. Undo the shell's signal dispositions (ignored signals survive exec!).
  for (int s : {SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU, SIGCHLD, SIGPIPE})
    signal(s, SIG_DFL);

  // 3. Background jobs must not steal the shell's input: stdin <- /dev/null
  //    (an explicit "< file" below overrides this).
  if (cmd.background) {
    int nul = open("/dev/null", O_RDONLY);
    if (nul >= 0) { dup2(nul, STDIN_FILENO); close(nul); }
  }

  // 4. Redirections.
  for (const Redirect& r : cmd.redirs) {
    int fd = open_redirect(r);
    if (fd < 0) _exit(1);
    dup2(fd, r.fd);
    close(fd);
  }

  // 5. Built-in with '&': run it here and exit with its status.
  if (fn) {
    int st = fn(*this, cmd.argv);
    fflush(stdout);
    fflush(stderr);
    _exit(st);
  }

  // 6. Replace the process image.
  std::vector<char*> args;
  for (const auto& a : cmd.argv) args.push_back(const_cast<char*>(a.c_str()));
  args.push_back(nullptr);
  execvp(args[0], args.data());

  int e = errno;  // exec failed
  if (e == ENOENT && cmd.argv[0].find('/') == std::string::npos)
    fprintf(stderr, "myshell: %s: command not found\n", args[0]);
  else
    fprintf(stderr, "myshell: %s: %s\n", args[0], strerror(e));
  _exit(e == ENOENT ? 127 : 126);  // same convention as POSIX shells
}

void Shell::launch(const Command& cmd, BuiltinFn fn) {
  // Flush buffered output first, otherwise the child would inherit a copy of
  // it and print it a second time.
  fflush(stdout);
  fflush(stderr);

  pid_t pid = fork();
  if (pid < 0) {
    fprintf(stderr, "myshell: fork failed: %s\n", strerror(errno));
    last_status = 1;
    return;
  }
  if (pid == 0) child_exec(cmd, fn);

  // Parent.  Ignore errors: EACCES just means the child already exec'd and
  // has already put itself in the group.
  setpgid(pid, pid);
  Job& job = jobs.add(pid, pid, cmd.text, !cmd.background);

  if (cmd.background) {
    printf("[%d] %d\n", job.id, (int)pid);
    fflush(stdout);
    last_status = 0;
  } else {
    last_status = wait_foreground(job);
  }
}

// ---------------------------------------------------------------------------
// Waiting and state tracking
// ---------------------------------------------------------------------------

// Fold a wait4() status word into the job record.
void Shell::update_from_status(Job& job, int status, const struct rusage& ru) {
  job.cpu_user_ms = tv_ms(ru.ru_utime);
  job.cpu_sys_ms = tv_ms(ru.ru_stime);
  if (WIFSTOPPED(status)) {
    job.state = JobState::Stopped;
    job.stop_signal = WSTOPSIG(status);
    job.stop_count++;
  } else if (WIFEXITED(status)) {
    job.state = JobState::Done;
    job.exit_status = WEXITSTATUS(status);
    job.finish = Job::Clock::now();
  } else if (WIFSIGNALED(status)) {
    job.state = JobState::Terminated;
    job.term_signal = WTERMSIG(status);
    job.finish = Job::Clock::now();
  }
}

// Block until the foreground job exits or stops; hand the terminal back.
int Shell::wait_foreground(Job& job) {
  const int id = job.id;
  give_terminal_to(job.pgid);

  int status = 0;
  struct rusage ru{};
  pid_t r;
  do {
    r = wait4(job.pid, &status, WUNTRACED, &ru);
  } while (r < 0 && errno == EINTR);

  give_terminal_to(shell_pgid_);                 // shell is foreground again
  if (interactive_) tcsetattr(tty_fd_, TCSADRAIN, &shell_tmodes_);  // undo terminal mode changes

  if (r < 0) {  // child vanished (should not happen)
    fprintf(stderr, "myshell: wait4: %s\n", strerror(errno));
    job.state = JobState::Done;
    job.exit_status = 1;
    job.finish = Job::Clock::now();
    jobs.retire(id);
    return 1;
  }
  update_from_status(job, status, ru);

  if (job.state == JobState::Stopped) {          // Ctrl-Z or SIGSTOP: keep the job, move it to the background
    job.foreground = false;
    jobs.touch(job);
    if (interactive_) fputc('\n', stdout);
    announce(job);
    return 128 + job.stop_signal;
  }

  int rc;
  if (job.state == JobState::Done) {
    rc = job.exit_status;
  } else {                                        // killed by a signal
    rc = 128 + job.term_signal;
    if (job.term_signal == SIGINT) {
      if (interactive_) fputc('\n', stdout);
    } else if (job.term_signal != SIGPIPE) {
      fprintf(stderr, "myshell: pid %d terminated by signal %d (%s): %s\n", (int)job.pid,
              job.term_signal, strsignal(job.term_signal), job.command.c_str());
    }
  }
  job.notified = true;
  jobs.retire(id);
  return rc;
}

// Collect status changes of background jobs without blocking.
void Shell::reap_children(bool notify) {
  for (;;) {
    int status = 0;
    struct rusage ru{};
    pid_t pid = wait4(-1, &status, WNOHANG | WUNTRACED, &ru);
    if (pid <= 0) break;                         // nothing changed (0) or no children (-1)
    Job* j = jobs.find_by_pid(pid);
    if (!j) continue;
    update_from_status(*j, status, ru);
    if (j->state == JobState::Stopped) {
      jobs.touch(*j);
      if (notify) announce(*j);
    } else if (j->finished()) {
      if (notify) announce(*j);
      j->notified = true;
      jobs.retire(j->id);
    }
  }
}

// After signalling a job, give the kernel a moment to deliver it so the next
// command sees the new state (keeps script output deterministic).
void Shell::wait_for_change(int id, JobState old_state, int timeout_ms) {
  for (int i = 0; i < timeout_ms; ++i) {
    reap_children(true);
    Job* j = jobs.find_by_id(id);
    if (!j || j->state != old_state) return;
    usleep(1000);
  }
}

// "[1]+  Done                     sleep 2"
void Shell::announce(const Job& job) {
  char mark = ' ';
  if (&job == jobs.current()) mark = '+';
  else if (&job == jobs.previous()) mark = '-';

  char desc[64];
  switch (job.state) {
    case JobState::Done:
      if (job.exit_status == 0) snprintf(desc, sizeof desc, "Done");
      else snprintf(desc, sizeof desc, "Exit %d", job.exit_status);
      break;
    case JobState::Terminated: {
      const char* n = signal_short_name(job.term_signal);
      if (job.term_signal == SIGTERM) snprintf(desc, sizeof desc, "Terminated");
      else if (job.term_signal == SIGKILL) snprintf(desc, sizeof desc, "Killed");
      else if (job.term_signal == SIGINT) snprintf(desc, sizeof desc, "Interrupt");
      else if (job.term_signal == SIGHUP) snprintf(desc, sizeof desc, "Hangup");
      else snprintf(desc, sizeof desc, "%s", n ? strsignal(job.term_signal) : "Signalled");
      break;
    }
    case JobState::Stopped: {
      const char* n = signal_short_name(job.stop_signal);
      snprintf(desc, sizeof desc, "Stopped (SIG%s)", n ? n : "?");
      break;
    }
    default:
      snprintf(desc, sizeof desc, "%s", to_string(job.state));
  }
  printf("[%d]%c  %-24s %s\n", job.id, mark, desc, job.command.c_str());
  fflush(stdout);
}

// ---------------------------------------------------------------------------
// fg / bg support
// ---------------------------------------------------------------------------
Job* Shell::resolve_job(const std::string& spec, std::string& err) {
  std::string s = spec;
  if (!s.empty() && s[0] == '%') s.erase(0, 1);

  Job* j = nullptr;
  if (s.empty() || s == "+" || s == "%") {
    j = jobs.current();
    if (!j) err = "no current job";
  } else if (s == "-") {
    j = jobs.previous();
    if (!j) err = "no previous job";
  } else {
    for (char c : s)
      if (c < '0' || c > '9') { err = "invalid job specification: " + spec; return nullptr; }
    if (s.size() > 9) { err = "no such job: " + spec; return nullptr; }
    j = jobs.find_by_id(std::stoi(s));
    if (!j) err = "no such job: " + spec;
  }
  return j;
}

int Shell::foreground_job(Job& job) {
  if (job.state == JobState::Stopped) {
    kill(-job.pgid, SIGCONT);                    // resume the whole group
    job.state = JobState::Running;
  }
  job.foreground = true;
  jobs.touch(job);
  printf("%s\n", job.command.c_str());           // echo the command like bash does
  fflush(stdout);
  return wait_foreground(job);
}

int Shell::background_job(Job& job) {
  if (job.state != JobState::Stopped) {
    fprintf(stderr, "bg: job %d already in background\n", job.id);
    return 0;
  }
  kill(-job.pgid, SIGCONT);
  job.state = JobState::Running;
  job.foreground = false;
  jobs.touch(job);
  printf("[%d]%c %s &\n", job.id, &job == jobs.current() ? '+' : ' ', job.command.c_str());
  fflush(stdout);
  return 0;
}

// ---------------------------------------------------------------------------
// Shutdown: never leave orphan children behind.
// ---------------------------------------------------------------------------
void Shell::cleanup_jobs() {
  auto act = jobs.active();
  if (act.empty()) return;
  fprintf(stderr, "myshell: terminating %zu remaining job(s)\n", act.size());
  for (Job* j : act) {
    kill(-j->pgid, SIGHUP);
    if (j->state == JobState::Stopped) kill(-j->pgid, SIGCONT);  // a stopped process can't act on SIGHUP
  }
  for (Job* j : act) {
    int status = 0;
    bool reaped = false;
    for (int i = 0; i < 300 && !reaped; ++i) {   // ~300 ms grace period
      reaped = waitpid(j->pid, &status, WNOHANG) != 0;
      if (!reaped) usleep(1000);
    }
    if (!reaped) {                               // ignored SIGHUP: escalate
      kill(-j->pgid, SIGKILL);
      waitpid(j->pid, &status, 0);
    }
  }
}
