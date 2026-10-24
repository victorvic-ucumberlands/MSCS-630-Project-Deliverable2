// scheduler.hpp - operating-system style CPU scheduler for REAL processes.
//
// What this module does
// ---------------------
// The user submits ordinary shell commands to the scheduler instead of running
// them directly:
//
//     sched add -p 3 cpuburn A 600
//     sched add -p 1 -t 300 md5sum bigfile
//     sched run rr -q 100
//
// `sched run` then makes the shell behave like the CPU scheduler of an
// operating system.  Every task is a real child process created with
// fork()/execvp(), and the shell decides which one of them may use the CPU at
// any instant:
//
//   * a task waiting in the ready queue is a genuinely stopped process
//     (SIGSTOP, kernel state 'T'), so it makes no progress at all;
//   * the task that holds the CPU has been continued with SIGCONT and runs
//     normally (kernel state 'R'/'S');
//   * only ONE managed task is continued at a time, which is how a
//     single-CPU system behaves.
//
// This is real preemptive multitasking driven by the shell: the processes are
// real, their output is real, and the waiting / turnaround / response times in
// the report are measured from the system clock, not calculated from declared
// burst lengths.
//
// Timers
// ------
// The quantum is enforced with a timer.  While a task runs, the scheduler
// blocks in sigtimedwait() with a timeout equal to the rest of the task's time
// slice.  Exactly one of three things ends the wait:
//
//   * SIGCHLD   - the running task exited (or was stopped/killed): it leaves
//                 the queue and the next task is dispatched immediately,
//                 without waiting for the rest of its slice;
//   * timeout   - the slice is used up: the task is preempted with SIGSTOP and
//                 goes to the back of the queue (Round-Robin);
//   * SIGINT    - the user pressed Ctrl-C: the run is abandoned cleanly.
//
// In priority mode the same timer is used to wake the scheduler at the next
// task's arrival time, so a higher-priority task that appears while a
// lower-priority task is running preempts it at that instant.
//
// Policies
// --------
//   FCFS        - FIFO queue, no time slice (baseline; run to completion)
//   RoundRobin  - FIFO queue + configurable quantum
//   Priority    - binary heap (std::priority_queue) ordered by priority, ties
//                 broken first-come-first-served; preemptive by default
#pragma once

#include <sys/types.h>

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "job.hpp"

enum class PolicyKind { FCFS, RoundRobin, Priority };

// ---------------------------------------------------------------------------
// A task = one real command plus its process-control block
// ---------------------------------------------------------------------------
struct SchedTask {
  // ---- what the user submitted -----------------------------------------
  std::vector<std::string> argv;  // the command to exec
  std::string out_path;           // -o FILE: task's stdout+stderr (empty = inherit)
  long arrival_ms = 0;            // -t MS: enters the system this long after the run starts

  // ---- process-control block (job.hpp) ---------------------------------
  // Holds id, command text, priority, pid/pgid, state, the arrival/first_run/
  // finish timestamps and the accounting counters.  Reusing `Job` means the
  // scheduler and the Deliverable 1 job table describe a process the same way.
  Job pcb;

  // ---- scheduler bookkeeping -------------------------------------------
  long slice_used_ms = 0;         // time already used inside the current quantum
  bool spawned = false;           // the process has been created
  bool aborted = false;           // killed by the shell because the run was cut short
};

// ---------------------------------------------------------------------------
// Configuration of one run
// ---------------------------------------------------------------------------
struct SchedConfig {
  PolicyKind policy = PolicyKind::RoundRobin;
  long quantum_ms = 100;        // Round-Robin time slice
  bool preemptive = true;       // Priority: preempt when a better task arrives
  long switch_cost_ms = 0;      // extra delay charged on every context switch
  long budget_ms = 120000;      // safety net: give up after this much wall time
  bool trace = true;            // print the scheduling event log
  bool verbose = false;         // add per-task state (including the kernel's own) to the log
};

// ---------------------------------------------------------------------------
// Scheduling policies: the "which task runs next" decision
// ---------------------------------------------------------------------------
class SchedPolicy {
 public:
  virtual ~SchedPolicy() = default;
  // A task became ready (it arrived, or it lost the CPU).
  virtual void enqueue(SchedTask* t) = 0;
  // Remove and return the task that should run next (nullptr if none ready).
  virtual SchedTask* pick() = 0;
  // Longest time this task may hold the CPU; 0 = until it blocks or finishes.
  virtual long time_slice(const SchedTask&) const { return 0; }
  // Must the running task give up the CPU right now for a better one?
  virtual bool should_preempt(const SchedTask&) const { return false; }
  // The ready queue in service order (for the event log).
  virtual std::vector<const SchedTask*> snapshot() const = 0;
  virtual bool show_priority() const { return false; }
};

std::unique_ptr<SchedPolicy> make_policy(const SchedConfig& cfg);

// ---------------------------------------------------------------------------
// Result of a run
// ---------------------------------------------------------------------------
struct GanttSegment {
  enum Kind { Task, Idle, Switch };
  Kind kind = Task;
  int task = -1;        // index into SchedResult::tasks (Task only)
  long start = 0, end = 0;
};

struct SchedResult {
  SchedConfig cfg;
  std::vector<SchedTask> tasks;       // final state of every task
  std::vector<GanttSegment> gantt;
  bool gantt_truncated = false;
  long makespan_ms = 0;               // wall time from the start of the run to the last exit
  long busy_ms = 0;                   // time a task held the CPU
  long idle_ms = 0;                   // no task ready to run
  long overhead_ms = 0;               // charged context-switch cost
  int context_switches = 0;           // dispatches that changed the running task
  int preemptions = 0;                // SIGSTOPs issued (quantum expiry or priority)
  bool interrupted = false;           // Ctrl-C, or SIGTERM/SIGHUP when run as a background job
  int  abort_signal = 0;              // which signal cut the run short
  bool over_budget = false;           // hit cfg.budget_ms

  int finished_count() const;
  double avg_waiting_ms() const;      // mean of (turnaround - time holding the CPU)
  double avg_turnaround_ms() const;   // mean of (exit - arrival)
  double avg_response_ms() const;     // mean of (first dispatch - arrival)
  double cpu_utilization() const;     // busy / makespan
  double throughput_per_s() const;    // completed tasks per second
};

// ---------------------------------------------------------------------------
// The scheduler: owns the submitted workload and the last result
// ---------------------------------------------------------------------------
class Scheduler {
 public:
  static constexpr size_t kMaxTasks = 64;        // real processes, so keep it modest
  static constexpr long kMaxArrivalMs = 600000;  // 10 minutes
  static constexpr long kMaxBudgetMs = 900000;   // 15 minutes

  // Submit a command.  Returns "" on success or an error message.
  std::string add(const SchedTask& t);
  bool remove(int id);
  void clear() { tasks_.clear(); next_id_ = 1; }
  const std::vector<SchedTask>& workload() const { return tasks_; }
  bool empty() const { return tasks_.empty(); }

  // Run the whole workload under `cfg`, creating and scheduling real
  // processes.  The event log goes to `out`.  The submitted workload is left
  // untouched, so the same commands can be replayed under another policy.
  SchedResult run(const SchedConfig& cfg, FILE* out);

  std::optional<SchedResult> last;    // report of the most recent run

  static void print_report(const SchedResult& r, FILE* out);
  static std::string describe(const SchedConfig& cfg);

 private:
  std::vector<SchedTask> tasks_;
  int next_id_ = 1;
};
