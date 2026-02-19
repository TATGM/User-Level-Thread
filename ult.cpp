#include "ult.h"

#include <iostream>
#include <csignal>
#include <cstring>
#include <deque>
#include <string>
#include <map>
#include <set>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>
#include <cstdio>
#include <ctime>

namespace {

constexpr size_t kStackSize = 64 * 1024;
constexpr int kMainThreadId = 0;

enum class ThreadState {
  RUNNING,
  READY,
  BLOCKED,
  TERMINATED,
};

enum class WaitType {
  NONE,
  MUTEX,
  JOIN,
};

struct Thread {
  ult_tid_t tid = -1;
  ucontext_t context{};
  ThreadState state = ThreadState::READY;
  void* return_value = nullptr;
  void* stack = nullptr;
  std::vector<ult_tid_t> joiners;
  WaitType wait_type = WaitType::NONE;
  void* wait_object = nullptr;
  bool in_ready_queue = false;
  uintptr_t stack_low = 0;
  uintptr_t stack_high = 0;
};

std::map<ult_tid_t, Thread> global_threads;
std::deque<ult_tid_t> global_ready_queue;
std::deque<ult_tid_t> global_zombie_queue;
ult_tid_t global_current_thread_id = kMainThreadId;
int global_next_thread_id = 1;
bool global_initialized = false;
volatile sig_atomic_t global_report_deadlock = 0;
volatile sig_atomic_t global_need_reschedule = 0;
volatile sig_atomic_t global_in_preempt = 0;
volatile uintptr_t global_stack_low = 0;
volatile uintptr_t global_stack_high = 0;

sigset_t global_timer_mask;

class SignalBlocker {
 public:
  SignalBlocker() { sigprocmask(SIG_BLOCK, &global_timer_mask, &old_mask_); }
  ~SignalBlocker() { sigprocmask(SIG_SETMASK, &old_mask_, nullptr); }

 private:
  sigset_t old_mask_{};
};

void enqueue_ready(ult_tid_t thread_id) {
  if (thread_id < 0) {
    return;
  }
  auto id_thread = global_threads.find(thread_id);
  if (id_thread == global_threads.end()) {
    return;
  }
  Thread& thread = id_thread->second;
  if (thread.state == ThreadState::READY && !thread.in_ready_queue) {
    thread.in_ready_queue = true;
    global_ready_queue.push_back(thread_id);
  }
}

void reap_zombies() {
  while (!global_zombie_queue.empty()) {
    ult_tid_t thread_id = global_zombie_queue.front();
    global_zombie_queue.pop_front();
    auto id_thread = global_threads.find(thread_id);
    if (id_thread == global_threads.end()) {
      continue;
    }
    Thread& thread = id_thread->second;
    if (thread.stack) {
      delete[] reinterpret_cast<char*>(thread.stack);
      thread.stack = nullptr;
    }
  }
}

void schedule_next();

void thread_trampoline(uintptr_t function_pointer, uintptr_t argument_pointer) {
  auto function = reinterpret_cast<void* (*)(void*)>(function_pointer);
  void* argument = reinterpret_cast<void*>(argument_pointer);
  void* return_argument = function(argument);
  ult_exit(return_argument);
}

void report_deadlocks() {
  global_report_deadlock = 0;

  std::map<ult_tid_t, ult_tid_t> wait_for;
  for (const auto& [thread_id, thread] : global_threads) {
    if (thread.state != ThreadState::BLOCKED) {
      continue;
    }
    if (thread.wait_type == WaitType::MUTEX) {
      auto* mutex = reinterpret_cast<ult_mutex*>(thread.wait_object);
      if (mutex && mutex->locked) {
        wait_for[thread_id] = mutex->owner;
      }
    } else if (thread.wait_type == WaitType::JOIN) {
      auto* target = reinterpret_cast<ult_tid_t*>(thread.wait_object);
      if (target) {
        wait_for[thread_id] = *target;
      }
    }
  }

  std::set<ult_tid_t> reported;
  std::string out = "Deadlock report (SIGQUIT):\n";
  for (const auto& [start, _] : wait_for) {
    if (reported.count(start)) {
      continue;
    }
    std::set<ult_tid_t> seen;
    ult_tid_t cur = start;
    while (wait_for.count(cur)) {
      if (seen.count(cur)) {
        out += "  cycle detected starting at tid " + std::to_string(cur) + "\n";
        for (auto tid : seen) {
          reported.insert(tid);
        }
        break;
      }
      seen.insert(cur);
      cur = wait_for[cur];
    }
  }

  if (out == "Deadlock report (SIGQUIT):\n") {
    out += "  no cycles detected\n";
  }

  write(STDERR_FILENO, out.c_str(), out.size());
}

void maybe_report_deadlocks() {
  if (global_report_deadlock) {
    report_deadlocks();
  }
}

void check_preempt() {
  if (!global_need_reschedule) {
    return;
  }
  global_need_reschedule = 0;
  schedule_next();
}

void preempt_trampoline() {
  global_in_preempt = 1;
  check_preempt();
  global_in_preempt = 0;
}

void sigvtalrm_handler(int, siginfo_t*, void* ucontext) {
  global_need_reschedule = 1;
#if defined(__x86_64__)
  if (global_in_preempt) {
    return;
  }
  auto* context = reinterpret_cast<ucontext_t*>(ucontext);
  greg_t rip = context->uc_mcontext.gregs[REG_RIP];
  if (rip == reinterpret_cast<greg_t>(preempt_trampoline)) {
    return;
  }
  greg_t rsp = context->uc_mcontext.gregs[REG_RSP];
  uintptr_t stack_low = global_stack_low;
  uintptr_t stack_high = global_stack_high;
  if (stack_low == 0 || stack_high == 0) {
    return;
  }
  uintptr_t new_rsp = static_cast<uintptr_t>(rsp) - sizeof(uintptr_t);
  if (new_rsp < stack_low || new_rsp + sizeof(uintptr_t) > stack_high) {
    return;
  }
  auto* stack = reinterpret_cast<uintptr_t*>(rsp);
  stack -= 1;
  *stack = static_cast<uintptr_t>(rip);
  context->uc_mcontext.gregs[REG_RSP] = reinterpret_cast<greg_t>(stack);
  context->uc_mcontext.gregs[REG_RIP] = reinterpret_cast<greg_t>(preempt_trampoline);
#else
  (void)ucontext;
#endif
}

void sigquit_handler(int) {
  global_report_deadlock = 1;
  global_need_reschedule = 1;
}

void init_signals() {
  sigemptyset(&global_timer_mask);
  sigaddset(&global_timer_mask, SIGVTALRM);

  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = sigvtalrm_handler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGVTALRM, &sa, nullptr);

  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = sigquit_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGQUIT, &sa, nullptr);

  signal(SIGPIPE, SIG_IGN);
}

void start_timer(unsigned int quantum_us) {
  itimerval tv{};
  tv.it_interval.tv_sec = quantum_us / 1000000;
  tv.it_interval.tv_usec = quantum_us % 1000000;
  tv.it_value = tv.it_interval;
  setitimer(ITIMER_VIRTUAL, &tv, nullptr);
}

void schedule_next() {
  SignalBlocker guard;
  maybe_report_deadlocks();
  reap_zombies();

  if (global_ready_queue.empty()) {
    return;
  }

  ult_tid_t previous_thread_id = global_current_thread_id;
  Thread& previous = global_threads[previous_thread_id];
  if (previous.state == ThreadState::RUNNING) {
    previous.state = ThreadState::READY;
    enqueue_ready(previous_thread_id);
  }

  ult_tid_t next_thread_id = global_ready_queue.front();
  global_ready_queue.pop_front();
  Thread& next = global_threads[next_thread_id];
  next.in_ready_queue = false;
  next.state = ThreadState::RUNNING;
  global_current_thread_id = next_thread_id;
  global_stack_low = next.stack_low;
  global_stack_high = next.stack_high;

  if (previous_thread_id == next_thread_id) {
    return;
  }

  swapcontext(&previous.context, &next.context);
  global_stack_low = previous.stack_low;
  global_stack_high = previous.stack_high;
}

void ensure_initialized() {
  if (global_initialized) {
    return;
  }
  ult_init(5000);
}

void wake_joiners(Thread& t) {
  for (ult_tid_t tid : t.joiners) {
    Thread& waiter = global_threads[tid];
    if (waiter.state == ThreadState::BLOCKED) {
      waiter.state = ThreadState::READY;
      waiter.wait_type = WaitType::NONE;
      waiter.wait_object = nullptr;
      enqueue_ready(tid);
    }
  }
  t.joiners.clear();
}

void maybe_reap_thread(ult_tid_t tid) {
  auto it = global_threads.find(tid);
  if (it == global_threads.end()) {
    return;
  }
  Thread& t = it->second;
  if (t.state != ThreadState::TERMINATED || !t.stack) {
    return;
  }
  delete[] reinterpret_cast<char*>(t.stack);
  t.stack = nullptr;
  if (t.joiners.empty()) {
    global_threads.erase(it);
  }
}

}  // namespace

// initializes the thread library and its internal structures
int ult_init(unsigned int quantum_us) {
  if (global_initialized) {
    return 0;
  }

  global_initialized = true;
  init_signals();
  start_timer(quantum_us);

  Thread main_thread;
  main_thread.tid = kMainThreadId;
  main_thread.state = ThreadState::RUNNING;
  getcontext(&main_thread.context);
  main_thread.stack_low = 0;
  main_thread.stack_high = 0;
  global_threads[kMainThreadId] = main_thread;
  global_current_thread_id = kMainThreadId;
  global_stack_low = 0;
  global_stack_high = 0;

  return 0;
}

// creates a new thread
int ult_create(ult_tid_t* tid, void* (*start_routine)(void*), void* arg) {
  ensure_initialized();
  if (!tid || !start_routine) {
    return -1;
  }
  check_preempt();

  SignalBlocker guard;
  Thread thread;
  thread.tid = global_next_thread_id++;
  thread.state = ThreadState::READY;
  thread.stack = new char[kStackSize];
  thread.stack_low = reinterpret_cast<uintptr_t>(thread.stack);
  thread.stack_high = thread.stack_low + kStackSize;

  getcontext(&thread.context);
  thread.context.uc_stack.ss_sp = thread.stack;
  thread.context.uc_stack.ss_size = kStackSize;
  thread.context.uc_link = nullptr;
  sigemptyset(&thread.context.uc_sigmask);
  makecontext(&thread.context, reinterpret_cast<void (*)()>(thread_trampoline), 2,
              reinterpret_cast<uintptr_t>(start_routine),
              reinterpret_cast<uintptr_t>(arg));

  global_threads[thread.tid] = thread;
  enqueue_ready(thread.tid);
  *tid = thread.tid;
  return 0;
}

// returns the value of the currently executing thread.
ult_tid_t ult_self() {
  ensure_initialized();
  check_preempt();
  return global_current_thread_id;
}

// suspends the calling thread until the specified thread_id terminates
int ult_join(ult_tid_t tid, void** return_value) {
  ensure_initialized();
  if (tid == global_current_thread_id) {
    return -1;
  }
  check_preempt();

  SignalBlocker guard;
  auto it = global_threads.find(tid);
  if (it == global_threads.end()) {
    return -1;
  }

  Thread& target = it->second;
  if (target.state == ThreadState::TERMINATED) {
    if (return_value) {
      *return_value = target.return_value;
    }
    maybe_reap_thread(tid);
    return 0;
  }

  Thread& self = global_threads[global_current_thread_id];
  self.state = ThreadState::BLOCKED;
  self.wait_type = WaitType::JOIN;
  self.wait_object = &target.tid;
  target.joiners.push_back(self.tid);

  schedule_next();

  if (return_value) {
    *return_value = target.return_value;
  }

  maybe_reap_thread(tid);
  return 0;
}

// terminates the calling thread
void ult_exit(void* return_value) {
  ensure_initialized();
  SignalBlocker guard;
  Thread& self = global_threads[global_current_thread_id];
  self.return_value = return_value;
  self.state = ThreadState::TERMINATED;

  wake_joiners(self);

  if (global_ready_queue.empty()) {
    auto it = global_threads.find(kMainThreadId);
    if (it != global_threads.end() && it->second.state != ThreadState::TERMINATED &&
        kMainThreadId != global_current_thread_id) {
      Thread& main_thread = it->second;
      main_thread.state = ThreadState::RUNNING;
      main_thread.in_ready_queue = false;
      global_current_thread_id = kMainThreadId;
      global_stack_low = main_thread.stack_low;
      global_stack_high = main_thread.stack_high;
      setcontext(&main_thread.context);
    }
    _exit(0);
  }

  if (self.stack) {
    global_zombie_queue.push_back(self.tid);
  }

  ult_tid_t next_thread_id = global_ready_queue.front();
  global_ready_queue.pop_front();
  Thread& next = global_threads[next_thread_id];
  next.in_ready_queue = false;
  next.state = ThreadState::RUNNING;
  global_current_thread_id = next_thread_id;
  global_stack_low = next.stack_low;
  global_stack_high = next.stack_high;
  setcontext(&next.context);
  _exit(0);
}

// voluntarily relinquishes the CPU
int ult_yield() {
  ensure_initialized();
  check_preempt();
  schedule_next();
  return 0;
}

// prepares the ult_mutex structure for use
int ult_mutex_init(ult_mutex* mutex) {
  if (!mutex) {
    return -1;
  }
  mutex->initialized = true;
  mutex->locked = false;
  mutex->owner = -1;
  mutex->waiters.clear();
  return 0;
}

// acquires the mutex or blocks the calling thread if it's already held
int ult_mutex_lock(ult_mutex* mutex) {
  ensure_initialized();
  if (!mutex || !mutex->initialized) {
    return -1;
  }
  check_preempt();

  SignalBlocker guard;
  if (!mutex->locked) {
    mutex->locked = true;
    mutex->owner = global_current_thread_id;
    return 0;
  }

  if (mutex->owner == global_current_thread_id) {
    return -1;
  }

  Thread& self = global_threads[global_current_thread_id];
  self.state = ThreadState::BLOCKED;
  self.wait_type = WaitType::MUTEX;
  self.wait_object = mutex;
  mutex->waiters.push_back(self.tid);

  schedule_next();
  return 0;
}

// releases the mutex and wakes up the next waiting thread
int ult_mutex_unlock(ult_mutex* mutex) {
  ensure_initialized();
  if (!mutex || !mutex->initialized || !mutex->locked) {
    return -1;
  }
  check_preempt();

  SignalBlocker guard;
  if (mutex->owner != global_current_thread_id) {
    return -1;
  }

  if (!mutex->waiters.empty()) {
    ult_tid_t next = mutex->waiters.front();
    mutex->waiters.pop_front();
    mutex->owner = next;

    Thread& waiter = global_threads[next];
    waiter.state = ThreadState::READY;
    waiter.wait_type = WaitType::NONE;
    waiter.wait_object = nullptr;
    enqueue_ready(next);
  } else {
    mutex->locked = false;
    mutex->owner = -1;
  }

  return 0;
}

// with mutex

static ult_mutex global_mutex;
static int global_counter = 0;
static int global_in_critical = 0;

void* worker_mutex(void* arg) {
  int id = *reinterpret_cast<int*>(arg);
  for (int i = 0; i < 5; ++i) {
    if (ult_mutex_lock(&global_mutex) != 0) {
      std::printf("worker %d failed to lock\n", id);
      return nullptr;
    }

    if (global_in_critical != 0) {
      std::printf("ERROR: worker %d entered while %d holds lock\n", id,
                  global_in_critical);
    }
    global_in_critical = id;

    int before = global_counter;
    global_counter = before + 1;
    std::printf("worker %d enter: counter %d -> %d\n", id, before, global_counter);

    ult_yield();  // Others will try to lock, but must wait.

    std::printf("worker %d exit\n", id);
    global_in_critical = 0;

    ult_mutex_unlock(&global_mutex);
    ult_yield();
  }
  return nullptr;
}

// without mutex

std::string format_timestamp() {
  timespec timestamp{};
  clock_gettime(CLOCK_REALTIME, &timestamp);
  tm tm_now{};
  localtime_r(&timestamp.tv_sec, &tm_now);
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%06ld",
                tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec,
                timestamp.tv_nsec / 1000);
  return std::string(buf);
}

FILE* open_worker_log(int id) {
  std::string name = "worker_" + std::to_string(id) + ".log";
  return std::fopen(name.c_str(), "w");
}

void* worker(void* arg) {
  int id = *reinterpret_cast<int*>(arg);
  FILE* log = open_worker_log(id);
  if (!log) {
    return nullptr;
  }
  for (int i = 0; i < 5; ++i) {
    std::string timestamp = format_timestamp();
    std::fprintf(log, "[%s] worker %d iteration %d\n", timestamp.c_str(), id, i);
    std::fflush(log);
    ult_yield();
    //usleep(1000000);
  }
  std::fclose(log);
  return reinterpret_cast<void*>(static_cast<intptr_t>(id * 10));
}

void* spinner(void*) {
  volatile unsigned long x = 0;
  while (x < 500000000UL)
  {
      x=x+1;
  }
  return nullptr;
}

void* printer(void*) {
  for (int i = 0; i < 10; i++) {
    std::string timestamp = format_timestamp();
    printf("tick %d time: %s\n", i, timestamp.c_str());
  }
  return nullptr;
}

int main() {
    ult_tid_t thread[100];
    
    int scenario=1;
    
    switch(scenario) {
    case 1: {
    ult_init(2000);
    ult_mutex_init(&global_mutex);

    int id_thread[100];

    for(int i=1; i<=3; i++)
    {
    id_thread[i]=i; thread[i]=id_thread[i];
    ult_create(&thread[i], worker_mutex, &id_thread[i]);
    }

    ult_join(thread[1], nullptr);
    ult_join(thread[2], nullptr);
    ult_join(thread[3], nullptr);

    std::printf("final counter: %d (expected %d)\n", global_counter, 3 * 5);
    break;
    }
    
    case 2: {
    ult_init(2000);

    int variables[100];

    for(int i=1; i<=10; i++) {
        variables[i]=i;
        ult_create(&thread[i], worker, &variables[i]);
    }
    ult_create(&thread[11], spinner, nullptr);
    ult_create(&thread[12], printer, nullptr);

    for(int i=1; i<=10; i++)
    {
    void* result[i] = {nullptr};
    ult_join(thread[i], &result[i]);
    printf("joined result: %ld\n", (long)result[i]);
    }
    
    ult_join(thread[11], nullptr);
    ult_join(thread[12], nullptr);

    break;
    }
    }
}
