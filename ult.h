#ifndef ULT_H
#define ULT_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

using ult_tid_t = int;

// Thread API
int ult_init(unsigned int quantum_us);
int ult_create(ult_tid_t* thread_id, void* (*start_routine)(void*), void* argument);
ult_tid_t ult_self();
int ult_join(ult_tid_t thread_id, void** return_value);
void ult_exit(void* return_value);
int ult_yield();

// Mutex API
struct ult_mutex {
  bool initialized = false;
  bool locked = false;
  ult_tid_t owner = -1;
  std::deque<ult_tid_t> waiters;
};

int ult_mutex_init(ult_mutex* mutex);
int ult_mutex_lock(ult_mutex* mutex);
int ult_mutex_unlock(ult_mutex* mutex);

#endif
