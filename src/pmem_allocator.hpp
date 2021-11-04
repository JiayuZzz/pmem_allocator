#pragma once

#include <fcntl.h>
#include <sys/mman.h>

#include <memory>
#include <set>
#include <vector>
#include <atomic>
#include <unordered_map>
#include <assert.h>
#include <thread>

#include "thread_manager.hpp"
#include "space_entry.hpp"

constexpr uint64_t kNullPmemOffset = UINT64_MAX;
constexpr uint64_t kMinMovableListSize = 8;

template<typename T>
class FixVector {
public:
    FixVector(uint64_t size) : size_(size) {
        data_ = new T[size];
    }

    FixVector() = delete;

    FixVector(const FixVector<T> &v) {
        size_ = v.size_;
        data_ = new T[size_];
        memcpy(data_, v.data_, size_ * sizeof(T));
    }

    ~FixVector() {
        if (data_ != nullptr) {
            delete[] data_;
        }
    }

    T &operator[](uint64_t index) {
        assert(index < size_);
        return data_[index];
    }

    uint64_t size() {
        return size_;
    }

private:
    T *data_;
    uint64_t size_;
};

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
            : pool_(max_classified_b_size + 1), spins_(max_classified_b_size + 1) {}

    // move a entry list of b_size free space entries to pool, "src" will be empty
    // after move
    void MoveEntryList(std::vector<void *> &src, uint32_t b_size);

    // try to fetch b_size free space entries from a entry list of pool to dst
    bool FetchEntryList(std::vector<void *> &dst, uint32_t b_size);

private:
    std::vector<std::vector<std::vector<void *>>> pool_;
    // Entry lists of a same block size guarded by a spin lock
    std::vector<SpinMutex> spins_;
};

using FreeList = FixVector<std::vector<void *>>;
using Segments = FixVector<PMemSpaceEntry>;
using Locks = FixVector<SpinMutex>;

// Manage allocation/de-allocation of PMem space at block unit
//
// PMem space consists of several segment, and a segment is consists of
// several blocks, a block is the minimal allocation unit of PMem space. The
// maximum allocated data size should smaller than a segment.
class PMEMAllocator {
public:
    ~PMEMAllocator();

    static PMEMAllocator *
    NewPMEMAllocator(const std::string &pmem_file, uint64_t pmem_size,
                     uint64_t num_segment_blocks, uint32_t block_size,
                     uint32_t max_access_threads, bool use_devdax_mode);

    // Allocate a PMem space, return offset and actually allocated space in bytes
    PMemSpaceEntry Allocate(uint64_t size);

    // Free a PMem space entry. The entry should be allocated by this allocator
    void Free(const PMemSpaceEntry &entry);

    inline void *offset2addr(uint64_t offset) {
        if (validate_offset(offset)) {
            return pmem_ + offset;
        }
        return nullptr;
    }

    // Populate PMem space so the following access can be faster
    // Warning! this will zero the entire PMem space
    void PopulateSpace();

    // Regularly execute by background thread
    void BackgroundWork();

private:
    PMEMAllocator(char *pmem, uint64_t pmem_size, uint64_t num_segment_blocks,
                  uint32_t block_size, uint32_t max_access_threads);

    inline bool MaybeInitAccessThread() {
        return thread_manager_->MaybeInitThread(access_thread);
    }

    inline uint64_t addr2offset(const void *addr) {
        if (addr) {
            uint64_t offset = (char *) addr - pmem_;
            if (validate_offset(offset)) {
                return offset;
            }
        }
        return kNullPmemOffset;
    }

    inline bool validate_offset(uint64_t offset) {
        return offset < pmem_size_ && offset != kNullPmemOffset;
    }

    // Write threads cache a dedicated PMem segment and a free space to
    // avoid contention
    struct ThreadCache {
        ThreadCache(uint32_t max_classified_block_size) : freelist(max_classified_block_size + 1),
                                                          segments(max_classified_block_size + 1),
                                                          locks(max_classified_block_size + 1) {
        }

        // A array of array to store freed space, the space size is aligned to block_size_, each array corresponding to a dedicated block size which is equal to its index
        FreeList freelist;
        // Thread own segments, each segment corresponding to a dedicated block size which is equal to its index
        Segments segments;
        // Protect freelist;
        Locks locks;

        char padding[64 - sizeof(freelist) - sizeof(segments) - sizeof(locks)];
    };

    static_assert(sizeof(ThreadCache) % 64 == 0);


    bool AllocateSegmentSpace(PMemSpaceEntry *segment_entry);

    static bool CheckDevDaxAndGetSize(const char *path, uint64_t *size);

    void init_data_size_2_block_size() {
        data_size_2_block_size_.resize(4096);
        for (size_t i = 0; i < data_size_2_block_size_.size(); i++) {
            data_size_2_block_size_[i] =
                    (i / block_size_) + (i % block_size_ == 0 ? 0 : 1);
        }
    }

    inline uint32_t size_2_block_size(uint32_t data_size) {
        if (data_size < data_size_2_block_size_.size()) {
            return data_size_2_block_size_[data_size];
        }
        return data_size / block_size_ + (data_size % block_size_ == 0 ? 0 : 1);
    }

    std::vector<ThreadCache> thread_cache_;
    const uint32_t block_size_;
    const uint64_t segment_size_;
    std::atomic<uint64_t> offset_head_;
    char *pmem_;
    uint64_t pmem_size_;
    std::shared_ptr<ThreadManager> thread_manager_;
    SpaceEntryPool pool_;
    // For quickly get corresponding block size of a requested data size
    std::vector<uint16_t> data_size_2_block_size_;
    std::vector<std::thread> bg_threads_;
    bool closing_;
};
