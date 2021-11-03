#include "pmem_allocator.hpp"
#include "string.h"

#include <thread>
#include <unistd.h>
#include "memkind.h"

int main() {
    int threads = 64;
    int block_size = 32;
    bool done = false;
    std::atomic<uint64_t> ops;
    memkind_t kind;
    memkind_create_pmem("/mnt/pmem0/memkind", 100ULL * 1024 * 1024 * 1024, &kind);
    PMEMAllocator *allocator = PMEMAllocator::NewPMEMAllocator("/mnt/pmem0/pool", 1ULL * 100 * 1024 * 1024 * 1024,
                                                               1024 * 1024 / block_size,
                                                               block_size, threads, false);
    std::vector<std::thread> ths;
    uint64_t cycle = 1024 * 1024;
    int cnt = 1024;
    auto bench = [&](int tid) {
        std::vector<PMemSpaceEntry> entries(cnt);
        std::vector<void *> pointers(cnt);
        for (int i = 1; i <= cycle; i++) {
            if (done) return;
            int allocate_size = (cycle * block_size) % 1025;

            for (int j = 0; j < cnt; j++) {
//                pointers[j] = malloc(allocate_size);
//                pointers[j] = memkind_malloc(kind, allocate_size);
                entries[j] = allocator->Allocate(allocate_size);
            }

            for (int j = 0; j < cnt; j++) {
//                free(pointers[j]);
//                memkind_free(kind, pointers[j]);
                allocator->Free(entries[j]);
            }
            ops += cnt;
        }
    };

    for (int i = 0; i < threads; i++) {
        ths.emplace_back(bench, i);
    }
    ops = 0;
    uint64_t elapsed_time = 0;
    uint64_t last_ops = 0;
    while (1) {
        sleep(1);
        elapsed_time++;
        uint64_t total_ops = ops.load(std::memory_order_relaxed);
        printf("last qps %lu/s, total qps %lu/s\n", total_ops - last_ops, total_ops / elapsed_time);
        last_ops = total_ops;
        if (elapsed_time == 30) {
            done = true;
            break;
        }
    }

    for (auto &t: ths) {
        t.join();
    }
}