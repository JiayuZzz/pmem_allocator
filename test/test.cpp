#include "string.h"

#include "memkind.h"
#include "pmem_allocator.hpp"
#include <atomic>
#include <functional>
#include <thread>
#include <unistd.h>
#include <vector>

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
    printf("last qps %lu/s, total qps %lu/s\n", total_ops - last_ops,
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
  int threads = 64;
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
    std::vector<PMemSpaceEntry> entries(cnt);
    std::vector<void *> pointers(cnt);
    for (int i = 1; i <= cycle; i++) {
      if (done)
        return;
      int allocate_size = (cycle * block_size) % 1025;

      for (int j = 0; j < cnt; j++) {
        //        pointers[j] = malloc(allocate_size);
        //                pointers[j] = memkind_malloc(kind, allocate_size);
        entries[j] = allocator->Allocate(allocate_size);
      }

      for (int j = 0; j < cnt; j++) {
        //        free(pointers[j]);
        //                memkind_free(kind, pointers[j]);
        allocator->Free(entries[j]);
      }
      ops += cnt;
    }
  };

  auto AllocateAccessFree = [&](int tid, std::atomic<uint64_t> &ops,
                                bool &done) {
    uint64_t cycle = 1024 * 1024 * 1024;
    int cnt = 1024;
    std::vector<PMemSpaceEntry> entries(cnt);
    std::vector<void *> pointers(cnt);
    for (int i = 1; i <= cycle; i++) {
      if (done)
        return;
      int allocate_size = (cycle * block_size) % 1025;

      for (int j = 0; j < cnt; j++) {
        //        pointers[j] = malloc(allocate_size);
        //                pointers[j] = memkind_malloc(kind, allocate_size);
        entries[j] = allocator->Allocate(allocate_size);
      }

      for (int j = 0; j < cnt; j++) {
        //        free(pointers[j]);
        //                memkind_free(kind, pointers[j]);
        allocator->Free(entries[j]);
      }
      ops += cnt;
    }
  };

  printf("Test Allocation / Free\n");
  LaunchTest(threads, benchmark_time, AllocateFree);
  printf("Test Allocation / Access / Free\n");
  LaunchTest(threads, benchmark_time, AllocateAccessFree);
  return 0;
}