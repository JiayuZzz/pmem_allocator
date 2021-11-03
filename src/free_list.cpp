/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2021 Intel Corporation
 */

#include "free_list.hpp"
#include "spin_mutex.hpp"
#include "thread_manager.hpp"

#include <mutex>
#include <libpmem.h>
#include <assert.h>


const uint32_t kMinMovableEntries = 8;

void Freelist::OrganizeFreeSpace() { MoveCachedListsToPool(); }

void Freelist::Push(const PMemSpaceEntry &entry) {
    assert(entry.size > 0);
    assert(entry.size % block_size_ == 0);
    auto b_size = entry.size / block_size_;
    auto &thread_cache = thread_cache_[access_thread.id];
    if (b_size >= thread_cache.active_entries.size()) {
        std::lock_guard<SpinMutex> lg(large_entries_spin_);
        large_entries_.emplace(entry);
    } else {
        std::lock_guard<SpinMutex> lg(thread_cache.spins[b_size]);
        thread_cache.active_entries[b_size].emplace_back(entry.addr);
    }
}

bool Freelist::Get(uint32_t size, PMemSpaceEntry *space_entry) {
    assert(size % block_size_ == 0);
    assert(space_entry != nullptr);
    auto b_size = size / block_size_;
    auto &thread_cache = thread_cache_[access_thread.id];
    for (uint32_t i = b_size; i < thread_cache.active_entries.size(); i++) {
        std::lock_guard<SpinMutex> lg(thread_cache.spins[i]);
        if (thread_cache.active_entries[i].size() == 0) {
            if (!active_pool_.FetchEntryList(thread_cache.active_entries[i], i)) {
                // no usable b_size free space entry
                continue;
            }
        }

        if (thread_cache.active_entries[i].size() != 0) {
            space_entry->addr = thread_cache.active_entries[i].back();
            space_entry->size = i * block_size_;
            thread_cache.active_entries[i].pop_back();
            return true;
        }
    }

    if (!large_entries_.empty()) {
        std::lock_guard<SpinMutex> lg(large_entries_spin_);
        while (!large_entries_.empty()) {
            auto large_entry = large_entries_.begin();
            auto entry_size = large_entry->size;
            assert(entry_size % block_size_ == 0);
            auto entry_b_size = entry_size / block_size_;
            if (entry_b_size >= b_size) {
                space_entry->addr = large_entry->addr;
                space_entry->size = large_entry->size;
                large_entries_.erase(large_entry);
                return true;
            } else {
                break;
            }
        }
    }
    return false;
}

void Freelist::MoveCachedListsToPool() {
    std::vector<void *> moving_list;
    for (auto &tc: thread_cache_) {
        for (size_t b_size = 1; b_size < tc.active_entries.size(); b_size++) {
            moving_list.clear();
            {
                std::lock_guard<SpinMutex> lg(tc.spins[b_size]);
                if (tc.active_entries[b_size].size() > 0) {
                    moving_list.swap(tc.active_entries[b_size]);
                }
            }

            if (moving_list.size() > 0) {
                active_pool_.MoveEntryList(moving_list, b_size);
            }
        }
    }
}

