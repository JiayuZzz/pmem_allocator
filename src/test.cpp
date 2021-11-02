#include "pmem_allocator.hpp"
#include "string.h"

#include <thread>
#include "memkind.h"

class Timer {
public:
    void Start() { clock_gettime(CLOCK_REALTIME, &start); }

    uint64_t End() {
        struct timespec end;
        clock_gettime(CLOCK_REALTIME, &end);
        return (end.tv_sec - start.tv_sec) * 1000000000 +
               (end.tv_nsec - start.tv_nsec);
    }

private:
    struct timespec start;
};

int main() {
    int threads = 32;
    int block_size = 32;
//    memkind_t kind;
//    memkind_create_pmem("/mnt/pmem0/memkind", 100ULL * 1024 * 1024 * 1024, &kind);
    PMEMAllocator *allocator = PMEMAllocator::NewPMEMAllocator("/mnt/pmem0/pool", 1ULL * 100 * 1024 * 1024 * 1024,
                                                               1024 * 1024,
                                                               block_size, threads, false);
    std::vector<std::thread> ths;
    uint64_t cycle = 1024 * 1024;
    int cnt = 128;
    std::vector<PMemSpaceEntry> entries(cnt);
    std::vector<void *> pointers(cnt);
    auto bench = [&](int tid) {
        for (int i = 1; i <= cycle; i++) {
            int allocate_size = (cycle * block_size) % 1025;

            for (int j = 0; j < cnt; j++) {
                entries[j] = allocator->Allocate(allocate_size);
            }

            for (int j = 0; j < cnt; j++) {
                allocator->Free(entries[j]);
            }
        }
    };

    Timer timer;
    timer.Start();

    for (int i = 0; i < threads; i++) {
        ths.emplace_back(bench, i);
    }
    for (auto &t: ths) {
        t.join();
    }
    uint64_t time_sec = timer.End() / 1000000000;
    uint64_t qps = cycle * cnt * threads / time_sec;
    printf("time sec: %lu, qps: %lu\n", time_sec, qps);
    return 0;
}