#pragma once

#include <atomic>
#include <memory>
#include <unordered_set>

#include "spin_mutex.hpp"

class ThreadManager;

struct Thread {
public:
  Thread() : id(-1), thread_manager(nullptr) {}

  ~Thread();

  void Release();

  int id;
  std::shared_ptr<ThreadManager> thread_manager;
};

class ThreadManager : public std::enable_shared_from_this<ThreadManager> {
public:
  ThreadManager(uint32_t max_threads) : max_threads_(max_threads), ids_(0) {}

  bool MaybeInitThread(Thread &t);

  void Release(const Thread &t);

private:
  std::atomic<uint32_t> ids_;
  std::unordered_set<uint32_t> usable_id_;
  uint32_t max_threads_;
  SpinMutex spin_;
};

extern thread_local Thread access_thread;