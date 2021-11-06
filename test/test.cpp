#include "string.h"

#include "memkind.h"
#include "pmem_allocator.hpp"
#include <atomic>
#include <functional>
#include <random>
#include <thread>
#include <unistd.h>
#include <vector>

// Return a string of length len with random characters in ['a', 'z']
inline std::string GetRandomString(size_t len) {
  static std::default_random_engine re;
  std::string str;
  str.reserve(len);
  for (size_t i = 0; i < len; i++)
    str.push_back('a' + re() % 26);
  return str;
}

void LaunchTest(
    int threads, uint32_t benchmark_time,
    std::function<void(int tid, std::atomic<uint64_t> &ops, bool &done)> func) {
  bool done = false;
  std::atomic<uint64_t> ops{0};
  std::vector<std::thread> ths;
  for (int i = 0; i < threads; i++) {
    ths.emplace_back(func, i, std::ref(ops), std::ref(done));
  }
  uint64_t elapsed_time = 0;
  uint64_t last_ops = 0;
  while (1) {
    sleep(1);
    elapsed_time++;
    uint64_t total_ops = ops.load(std::memory_order_relaxed);
    printf("last qps %lu/s, avs qps %lu/s\n", total_ops - last_ops,
           total_ops / elapsed_time);
    last_ops = total_ops;
    if (elapsed_time == benchmark_time) {
      done = true;
      break;
    }
  }

  for (auto &t : ths) {
    t.join();
  }
}

int main() {
  int threads = 32;
  int block_size = 32;
  uint64_t benchmark_time = 30;
  std::atomic<uint64_t> ops;
  memkind_t kind;
  memkind_create_pmem("/mnt/pmem0/memkind", 100ULL * 1024 * 1024 * 1024, &kind);
  PMemAllocator *allocator = PMemAllocator::NewPMemAllocator(
      "/mnt/pmem0/pool", 1ULL * 100 * 1024 * 1024 * 1024, threads, false);

  auto AllocateFree = [&](int tid, std::atomic<uint64_t> &ops, bool &done) {
    uint64_t cycle = 1024 * 1024 * 1024;
    int cnt = 1024;
    std::vector<void *> pointers(cnt);
    for (size_t i = 1; i <= cycle; i++) {
      if (done)
        return;
      size_t allocate_size = (i * block_size) % 1024 + 1;

      for (int j = 0; j < cnt; j++) {
        //        pointers[j] = malloc(allocate_size);
        //                pointers[j] = memkind_malloc(kind, allocate_size);
        pointers[j] = allocator->Allocate(allocate_size);
      }

      for (int j = 0; j < cnt; j++) {
        //        free(pointers[j]);
        //                memkind_free(kind, pointers[j]);
        allocator->Free(pointers[j]);
      }
      ops += cnt;
    }
  };

  auto AllocateAccess = [&](int tid, std::atomic<uint64_t> &ops, bool &done) {
    std::string random_str = GetRandomString(1024);
    uint64_t cycle = 1024 * 1024 * 1024;
    int cnt = 1024;
    std::vector<void *> pointers(cnt);
    for (size_t i = 1; i <= cycle; i++) {
      if (done)
        return;
      size_t allocate_size = (i * block_size) % 1024 + 1;

      for (int j = 0; j < cnt; j++) {
        //        pointers[j] = malloc(allocate_size);
        // pointers[j] = memkind_malloc(kind, allocate_size);
        pointers[j] = allocator->Allocate(allocate_size);
        memcpy(pointers[j], random_str.data(), allocate_size);
      }

      for (int j = 0; j < cnt; j++) {
        if (memcmp(pointers[j], random_str.data(), allocate_size) != 0) {
          fprintf(stderr, "data corrupted\n");
          exit(1);
        }
      }
      ops += cnt;
    }
  };

  printf("Test Allocation / Free\n");
  LaunchTest(threads, benchmark_time, AllocateFree);
  printf("Test Allocation / Access\n");
  LaunchTest(threads, benchmark_time, AllocateAccess);
  return 0;
}