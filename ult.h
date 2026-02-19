#ifndef ULT_H
#define ULT_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

using ult_tid_t = int;

// Thread API
int ult_init(unsigned int quantum_us); // initializes the thread library and its internal structures
int ult_create(ult_tid_t* thread_id, void* (*start_routine)(void*), void* argument); // creates a new thread
ult_tid_t ult_self(); // returns the value of the currently executing thread.
int ult_join(ult_tid_t thread_id, void** return_value); // suspends the calling thread until the specified thread_id terminates
void ult_exit(void* return_value); // terminates the calling thread
int ult_yield(); // voluntarily relinquishes the CPU

// Mutex API
struct ult_mutex {
  bool initialized = false;
  bool locked = false;
  ult_tid_t owner = -1;
  std::deque<ult_tid_t> waiters;
};

int ult_mutex_init(ult_mutex* mutex); // prepares the ult_mutex structure for use
int ult_mutex_lock(ult_mutex* mutex); // acquires the mutex or blocks the calling thread if it's already held
int ult_mutex_unlock(ult_mutex* mutex); // releases the mutex and wakes up the next waiting thread

#endif
