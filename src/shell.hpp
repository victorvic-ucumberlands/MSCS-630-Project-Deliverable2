// shell.hpp - the Shell object: read-eval loop, process creation and job control.
#pragma once

#include <sys/types.h>
#include <termios.h>

#include <cstdio>
#include <string>

#include "job.hpp"
#include "parser.hpp"

class Shell {
 public:
  // interactive : stdin is a terminal -> prompt, signal handling, terminal hand-off
  // echo_input  : print each script line before running it (for demo transcripts)
  Shell(bool interactive, bool echo_input);

  // Read-eval loop over `in` until EOF or `exit`.  Returns the shell's exit code.
  int run(FILE* in);

  // ---- state shared with the built-in commands -------------------------
  JobTable jobs;
  int last_status = 0;        // $? : status of the most recent command
  bool exit_requested = false;
  int exit_code = 0;
  bool exit_warned = false;   // "there are stopped jobs" already shown once
  pid_t shell_pid = 0;

  // ---- job-control operations used by jobs/fg/bg/kill -------------------
  // Resolve "%1", "1", "%%", "%+", "%-" (or empty = current job).  On failure
  // returns nullptr and fills `err`.
  Job* resolve_job(const std::string& spec, std::string& err);

  // Bring a job to the foreground, continue it if stopped, wait for it.
  int foreground_job(Job& job);
  // Continue a stopped job in the background.  Returns 0 on success.
  int background_job(Job& job);

  // Reap any children whose state changed; optionally print notifications.
  void reap_children(bool notify);
  // Poll (up to timeout_ms) until job `id` leaves state `old_state`.
  void wait_for_change(int id, JobState old_state, int timeout_ms);

  // Print the "[id]+ State  command" line for a job.
  void announce(const Job& job);

 private:
  void execute_line(const std::string& line);
  void run_command(const Command& cmd);
  void run_builtin_inline(int (*fn)(Shell&, const std::vector<std::string>&), const Command& cmd);
  void launch(const Command& cmd, int (*fn)(Shell&, const std::vector<std::string>&));
  [[noreturn]] void child_exec(const Command& cmd,
                               int (*fn)(Shell&, const std::vector<std::string>&));
  int wait_foreground(Job& job);
  void update_from_status(Job& job, int status, const struct rusage& ru);
  void cleanup_jobs();
  std::string lookup_var(const std::string& name);
  std::string prompt();
  void give_terminal_to(pid_t pgid);

  bool interactive_;
  bool echo_input_;
  int tty_fd_ = -1;
  pid_t shell_pgid_ = 0;
  struct termios shell_tmodes_{};
};
