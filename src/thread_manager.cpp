/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2021 Intel Corporation
 */

#include <assert.h>
#include <mutex>

#include "thread_manager.hpp"
#include "utils.hpp"

void Thread::Release() {
  assert(id == -1 || thread_manager != nullptr);
  if (thread_manager) {
    thread_manager->Release(*this);
    thread_manager = nullptr;
  }
  id = -1;
}

Thread::~Thread() { Release(); }

bool ThreadManager::MaybeInitThread(Thread &t) {
  if (t.id < 0) {
    if (!usable_id_.empty()) {
      std::lock_guard<SpinMutex> lg(spin_);
      if (!usable_id_.empty()) {
        auto it = usable_id_.begin();
        t.id = *it;
        usable_id_.erase(it);
        t.thread_manager = shared_from_this();
        return true;
      }
    }
    int id = ids_.fetch_add(1, std::memory_order_relaxed);
    if (id >= max_threads_) {
      return false;
    }
    t.id = id;
    t.thread_manager = shared_from_this();
  }
  return true;
}

void ThreadManager::Release(const Thread &t) {
  std::lock_guard<SpinMutex> lg(spin_);
  usable_id_.insert(t.id);
}

thread_local Thread access_thread;