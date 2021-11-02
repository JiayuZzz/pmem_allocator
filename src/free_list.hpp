/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2021 Intel Corporation
 */

#pragma once

#include <memory>
#include <vector>
#include <set>
#include <mutex>
#include <assert.h>

#include "space_entry.hpp"
#include "spin_mutex.hpp"


constexpr uint32_t kFreelistMaxClassifiedBlockSize = 255;
constexpr uint32_t kSpaceMapLockGranularity = 64;

class PMEMAllocator;

// free entry pool consists of three level vectors, the first level
// indicates different block size, each block size consists of several free
// space entry lists (the second level), and each list consists of several
// free space entries (the third level).
//
// For a specific block size, a write thread will move a entry list from the
// pool to its thread cache while no usable free space in the cache, or move a
// entry list to the pool while too many entries cached.
//
// Organization of the three level vectors:
//
// block size (1st level)   entry list (2nd level)   entries (3th level)
//     1   -----------------   list1    ------------   entry1
//                    |                         |---   entry2
//                    |-----   list2    ------------   entry1
//                                              |---   entry2
//                                              |---   entry3
//                              ...
//     2   -----------------   list1    ------------   entry1
//                    |                         |---   entry2
//                    |                         |---   entry3
//                    |-----   list2
//                              ...
//    ...
// max_block_size   --------   list1
//                    |-----   list2
class SpaceEntryPool {
public:
    SpaceEntryPool(uint32_t max_classified_b_size)
            : pool_(max_classified_b_size), spins_(max_classified_b_size) {}

    // move a entry list of b_size free space entries to pool, "src" will be empty
    // after move
    void MoveEntryList(std::vector<void *> &src, uint32_t b_size) {
        std::lock_guard<SpinMutex> lg(spins_[b_size]);
        assert(b_size < pool_.size());
        pool_[b_size].emplace_back();
        pool_[b_size].back().swap(src);
    }

    // try to fetch b_size free space entries from a entry list of pool to dst
    bool TryFetchEntryList(std::vector<void *> &dst, uint32_t b_size) {
        std::lock_guard<SpinMutex> lg(spins_[b_size]);
        if (pool_[b_size].size() != 0) {
            dst.swap(pool_[b_size].back());
            pool_[b_size].pop_back();
            return true;
        }
        return false;
    }

private:
    std::vector<std::vector<std::vector<void *>>> pool_;
    // Entry lists of a same block size guarded by a spin lock
    std::vector<SpinMutex> spins_;
};

class Freelist {
public:
    Freelist(uint32_t max_classified_b_size, uint32_t block_size, uint32_t num_threads)
            : block_size_(block_size),
              active_pool_(max_classified_b_size),
              thread_cache_(num_threads, max_classified_b_size) {}

    Freelist(uint32_t block_size, uint32_t num_threads)
            : Freelist(kFreelistMaxClassifiedBlockSize,
                       block_size, num_threads) {}

    // Add a space entry
    void Push(const PMemSpaceEntry &entry);

    // Request a at least "size" free space entry
    bool Get(uint32_t size, PMemSpaceEntry *space_entry);

    // Move cached free space list to space entry pool to balance usable space
    // of write threads
    //
    // Iterate every active entry lists of thread caches, move the list to
    // active_pool_
    void MoveCachedListsToPool();

    void OrganizeFreeSpace();

private:
    // Each thread caches some freed space entries in active_entries to
    // avoid contention. To balance free space entries among threads, a background
    // thread will regularly move active entries to entry pool which shared by all
    // threads.
    struct alignas(64) ThreadCache {
        ThreadCache(uint32_t max_classified_b_size)
                : active_entries(max_classified_b_size), spins(max_classified_b_size) {}

        // Entry size stored in block unit
        std::vector<std::vector<void *>> active_entries;
        std::vector<SpinMutex> spins;
    };

    class SpaceCmp {
    public:
        bool operator()(const PMemSpaceEntry &s1, const PMemSpaceEntry &s2) const {
            return s1.size > s2.size;
        }
    };

    const uint32_t block_size_;
    std::vector<ThreadCache> thread_cache_;
    SpaceEntryPool active_pool_;
    // Store all large free space entries that larger than max_classified_b_size_
    std::set<PMemSpaceEntry, SpaceCmp> large_entries_;
    SpinMutex large_entries_spin_;
};

