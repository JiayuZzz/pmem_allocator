/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2021 Intel Corporation
 */

#include <sys/sysmacros.h>
#include <thread>
#include <string.h>
#include <sys/stat.h>
#include <mutex>

#include "libpmem.h"
#include "pmem_allocator.hpp"
#include "thread_manager.hpp"

#define PATH_MAX 255

void SpaceEntryPool::MoveEntryList(std::vector<void *> &src, uint32_t b_size) {
    std::lock_guard<SpinMutex> lg(spins_[b_size]);
    assert(b_size < pool_.size());
    pool_[b_size].emplace_back();
    pool_[b_size].back().swap(src);
}

bool SpaceEntryPool::FetchEntryList(std::vector<void *> &dst, uint32_t b_size) {
    std::lock_guard<SpinMutex> lg(spins_[b_size]);
    if (pool_[b_size].size() != 0) {
        dst.swap(pool_[b_size].back());
        pool_[b_size].pop_back();
        return true;
    }
    return false;
}

PMEMAllocator::PMEMAllocator(char *pmem, uint64_t pmem_size,
                             uint64_t num_segment_blocks, uint32_t block_size,
                             uint32_t max_access_threads)
        : pmem_(pmem), block_size_(block_size), thread_cache_(max_access_threads, 32), pool_(32),
          segment_size_(num_segment_blocks * block_size), offset_head_(0),
          pmem_size_(pmem_size),
          thread_manager_(std::make_shared<ThreadManager>(max_access_threads)) {
    init_data_size_2_block_size();
}

void PMEMAllocator::Free(const PMemSpaceEntry &entry) {
    if (!MaybeInitAccessThread()) {
        fprintf(stderr, "too many thread access allocator!\n");
        std::abort();
    }

    if (entry.size > 0 && entry.addr != nullptr) {
        assert(entry.size % block_size_ == 0);
        auto b_size = entry.size / block_size_;
        auto &thread_cache = thread_cache_[access_thread.id];
        assert(b_size < thread_cache.freelist.size());
        std::lock_guard<SpinMutex> lg(thread_cache.locks[b_size]);
        thread_cache.freelist[b_size].emplace_back(entry.addr);
    }
}

void PMEMAllocator::PopulateSpace() {
    printf("Polulating PMem space ...\n");
    std::vector<std::thread> ths;

    int pu = 16; // 16 is a moderate concurrent number for writing PMem.
    for (int i = 0; i < pu; i++) {
        ths.emplace_back([=]() {
            uint64_t offset = pmem_size_ * i / pu;
            // To cover the case that mapped_size_ is not divisible by pu.
            uint64_t len = std::min(pmem_size_ / pu, pmem_size_ - offset);
            pmem_memset(pmem_ + offset, 0, len, PMEM_F_MEM_NONTEMPORAL);
        });
    }
    for (auto &t: ths) {
        t.join();
    }
    printf("Populating done\n");
}

PMEMAllocator::~PMEMAllocator() { pmem_unmap(pmem_, pmem_size_); }

PMEMAllocator *PMEMAllocator::NewPMEMAllocator(const std::string &pmem_file,
                                               uint64_t pmem_size,
                                               uint64_t num_segment_blocks,
                                               uint32_t block_size,
                                               uint32_t max_access_threads,
                                               bool use_devdax_mode) {
    int is_pmem;
    uint64_t mapped_size;
    char *pmem;
    // TODO jiayu: Should we clear map failed file?
    if (!use_devdax_mode) {
        if ((pmem = (char *) pmem_map_file(pmem_file.c_str(), pmem_size,
                                           PMEM_FILE_CREATE, 0666, &mapped_size,
                                           &is_pmem)) == nullptr) {
            fprintf(stderr, "PMem map file %s failed: %s\n", pmem_file.c_str(),
                    strerror(errno));
            return nullptr;
        }

        if (!is_pmem) {
            fprintf(stderr, "%s is not a pmem path\n", pmem_file.c_str());
            return nullptr;
        }
    } else {
        if (!CheckDevDaxAndGetSize(pmem_file.c_str(), &mapped_size)) {
            fprintf(stderr,
                    "CheckDevDaxAndGetSize %s failed device %s faild: %s\n",
                    pmem_file.c_str(), strerror(errno));
            return nullptr;
        }

        int flags = PROT_READ | PROT_WRITE;
        int fd = open(pmem_file.c_str(), O_RDWR, 0666);
        if (fd < 0) {
            fprintf(stderr, "Open devdax device %s faild: %s\n", pmem_file.c_str(),
                    strerror(errno));
            return nullptr;
        }

        if ((pmem = (char *) mmap(nullptr, pmem_size, flags, MAP_SHARED, fd, 0)) ==
            nullptr) {
            fprintf(stderr, "Mmap devdax device %s faild: %s\n", pmem_file.c_str(),
                    strerror(errno));
            return nullptr;
        }
    }

    if (mapped_size != pmem_size) {
        fprintf(stderr,
                "Pmem map file %s size %lu is not same as expected %lu\n",
                pmem_file.c_str(), mapped_size, pmem_size);
        return nullptr;
    }

    PMEMAllocator *allocator = nullptr;
    // We need to allocate a byte map in pmem allocator which require a large
    // memory, so we catch exception here
    try {
        allocator = new PMEMAllocator(pmem, pmem_size, num_segment_blocks,
                                      block_size, max_access_threads);
    } catch (std::bad_alloc &err) {
        fprintf(stderr, "Error while initialize PMEMAllocator: %s\n",
                err.what());
        return nullptr;
    }

    // num_segment_blocks and block_size are persisted and never changes.
    // No need to worry user modify those parameters so that records may be
    // skipped.
    size_t sz_wasted = pmem_size % (block_size * num_segment_blocks);
    if (sz_wasted != 0)
        fprintf(stderr,
                "Pmem file size not aligned with segment size, %llu space is wasted.\n",
                sz_wasted);
    printf("Map pmem space done\n");

    return allocator;
}

bool PMEMAllocator::AllocateSegmentSpace(PMemSpaceEntry *segment_entry) {
    uint64_t offset;
    while (1) {
        offset = offset_head_.load(std::memory_order_relaxed);
        if (offset < pmem_size_) {
            if (offset_head_.compare_exchange_strong(offset,
                                                     offset + segment_size_)) {
                if (offset > pmem_size_ - segment_size_) {
                    return false;
                }
                Free(*segment_entry);
                *segment_entry = PMemSpaceEntry{offset2addr(offset), segment_size_};
                return true;
            }
            continue;
        }
        return false;
    }
}

bool PMEMAllocator::CheckDevDaxAndGetSize(const char *path, uint64_t *size) {
    char spath[PATH_MAX];
    char npath[PATH_MAX];
    char *rpath;
    FILE *sfile;
    struct stat st;

    if (stat(path, &st) < 0) {
        fprintf(stderr, "stat file %s failed %s\n", path, strerror(errno));
        return false;
    }

    snprintf(spath, PATH_MAX, "/sys/dev/char/%d:%d/subsystem", major(st.st_rdev),
             minor(st.st_rdev));
    // Get the real path of the /sys/dev/char/major:minor/subsystem
    if ((rpath = realpath(spath, npath)) == 0) {
        fprintf(stderr, "realpath on file %s failed %s\n", spath,
                strerror(errno));
        return false;
    }

    // Checking the rpath is DAX device by compare
    if (strcmp("/sys/class/dax", rpath)) {
        return false;
    }

    snprintf(spath, PATH_MAX, "/sys/dev/char/%d:%d/size", major(st.st_rdev),
             minor(st.st_rdev));

    sfile = fopen(spath, "r");
    if (!sfile) {
        fprintf(stderr, "fopen on file %s failed %s\n", spath, strerror(errno));
        return false;
    }

    if (fscanf(sfile, "%lu", size) < 0) {
        fprintf(stderr, "fscanf on file %s failed %s\n", spath, strerror(errno));
        fclose(sfile);
        return false;
    }

    fclose(sfile);
    return true;
}

PMemSpaceEntry PMEMAllocator::Allocate(uint64_t size) {
    PMemSpaceEntry space_entry;
    if (!MaybeInitAccessThread()) {
        fprintf(stderr, "too many thread access allocator!\n");
        return space_entry;
    }
    uint32_t b_size = size_2_block_size(size);
    uint32_t aligned_size = b_size * block_size_;
    // Now the requested block size should smaller than segment size
    if (aligned_size > segment_size_ || aligned_size == 0) {
        fprintf(stderr, "allocating size is 0 or larger than PMem allocator segment\n");
        return space_entry;
    }
    auto &thread_cache = thread_cache_[access_thread.id];
    for (auto i = b_size; i < thread_cache.freelist.size(); i++) {
        std::lock_guard<SpinMutex> lg(thread_cache.locks[i]);
        if (thread_cache.segments[i].size < aligned_size) {
            // Fetch free list from pool
            if (thread_cache.freelist[i].empty()) {
//                pool_.FetchEntryList(thread_cache.freelist[i], i);
            }
            // Get space from free list
            if (thread_cache.freelist[i].size() > 0) {
                space_entry.addr = thread_cache.freelist[i].back();
                space_entry.size = i * block_size_;
                thread_cache.freelist[i].pop_back();
                break;
            }
            // Allocate a new segment for requesting block size
            if (!AllocateSegmentSpace(&thread_cache.segments[b_size])) {
                continue;
            } else {
                i = b_size;
            }
        }
        assert(thread_cache.segments[i].size >= aligned_size);
        space_entry.addr = thread_cache.segments[i].addr;
        space_entry.size = aligned_size;
        thread_cache.segments[i].size -= aligned_size;
        thread_cache.segments[i].addr = (char *) thread_cache.segments[i].addr + aligned_size;
        break;
    }
    return space_entry;
}

