#include "pmem_allocator.hpp"
#include "thread"

int main() {
    int threads = 32;
    int block_size = 32;
    PMEMAllocator *allocator = PMEMAllocator::NewPMEMAllocator("/mnt/pmem0/pool", 1ULL * 100 * 1024 * 1024, 32 * 1024,
                                                               block_size, threads, false);
    std::vector<std::thread> ths;
    auto bench = [&](int tid) {
        int cycle = 1024 * 1024;
        int cnt = 1024;
        for (int i = 1; i <= cycle; i++) {
            int allocate_size = (cycle * block_size) % 1025;
            std::vector<PMemSpaceEntry> entries;

            for (int j = 0; j < cnt; j++) {
                std::string a(allocate_size, (char) cnt);
                entries.push_back(allocator->Allocate(allocate_size));
                memcpy(entries.back().addr, a.data(), allocate_size);
            }

            for (int j = 0; j < cnt; j++) {
                std::string a(allocate_size, (char) cnt);
                if (memcmp(entries[j].addr, a.data(), a.size()) != 0) {
                    std::abort();
                }
                allocator->Free(entries[j]);
            }
        }
    };
    for (int i = 0; i < threads; i++) {
        ths.emplace(bench, i);
    }
    for (auto &t: ths) {
        t.join();
    }
    return 0;
}