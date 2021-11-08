/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2021 Intel Corporation
 */

#pragma once

#include <fcntl.h>
#include <sys/mman.h>

#include <assert.h>
#include <atomic>
#include <memory>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

#include "pmem_allocator.hpp"
#include "thread_manager.hpp"

constexpr uint64_t kPMemNull = UINT64_MAX;
constexpr uint64_t kMinMovableListSize = 8;

// Manage allocation/de-allocation of PMem space at block unit
//
// PMem space consists of several segment, and a segment is consists of
// several blocks, a block is the minimal allocation unit of PMem space. The
// maximum allocated data size should smaller than a segment.
class PMemAllocatorImpl : public PMemAllocator {
public:
  PMemAllocatorImpl(char *pmem, uint64_t pmem_size, uint32_t max_access_threads,
                    const PMemAllocatorConfig &config);

  PMemAllocatorImpl(char *pmem, uint64_t pmem_size, uint64_t segment_size,
                    uint32_t block_size, uint32_t max_access_threads);

  ~PMemAllocatorImpl();

  // Allocate a PMem space, return address and actually allocated space in bytes
  void *Allocate(uint64_t size) override;

  // Free a PMem space entry. The entry should be allocated by this allocator
  void Free(void *addr) override;

  // Release this access thread from the allocator, this will be auto-called
  // while the thread exit
  void Release() override { access_thread.Release(); }

  // Populate PMem space so the following access can be faster
  // Warning! this will zero the entire PMem space
  void PopulateSpace();

  // Regularly execute by background thread
  void BackgroundWork();

private:
  using FreeList = std::vector<void *>;

  struct Segment {
    Segment() : addr(nullptr), size(0) {}

    Segment(void *_addr, uint64_t _size) : addr(_addr), size(_size) {}

    void *addr;
    uint64_t size;
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
  // block size (1st level)   entry lists (2nd level)   entries (3th level)
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

    // move a entry list of b_size free space entries to pool, "src" will be
    // empty after move
    void MoveEntryList(std::vector<void *> &src, uint32_t b_size);

    // try to fetch b_size free space entries from a entry list of pool to dst
    bool FetchEntryList(std::vector<void *> &dst, uint32_t b_size);

  private:
    FixVector<std::vector<FreeList>> pool_;
    // Entry lists of a same block size guarded by a spin lock
    FixVector<SpinMutex> spins_;
  };

  inline bool MaybeInitAccessThread() {
    return thread_manager_->MaybeInitThread(access_thread);
  }

  inline void *Offset2Addr(uint64_t offset) {
    if (ValidateOffset(offset)) {
      return pmem_ + offset;
    }
    return nullptr;
  }

  inline uint64_t Addr2Offset(const void *addr) {
    if (addr) {
      uint64_t offset = (char *)addr - pmem_;
      if (ValidateOffset(offset)) {
        return offset;
      }
    }
    return kPMemNull;
  }

  inline void *Segment2Addr(uint64_t segment) {
    return Offset2Addr(segment * segment_size_);
  }

  inline uint64_t Addr2Segment(const void *addr) {
    uint64_t offset = Addr2Offset(addr);
    return offset == kPMemNull ? kPMemNull : offset / segment_size_;
  }

  inline bool ValidateOffset(uint64_t offset) {
    return offset < pmem_size_ && offset != kPMemNull;
  }

  // Write threads cache a list of dedicated PMem segments and free lists to
  // avoid contention
  struct alignas(64) ThreadCache {
    ThreadCache(uint32_t max_classified_block_size)
        : freelists(max_classified_block_size + 1),
          segments(max_classified_block_size + 1),
          locks(max_classified_block_size + 1) {}

    // A array of array to store freed space, the space size is aligned to
    // block_size_, each array corresponding to a dedicated block size which is
    // equal to its index
    FixVector<FreeList> freelists;
    // Thread own segments, each segment corresponding to a dedicated block size
    // which is equal to its index
    FixVector<Segment> segments;
    // Protect freelists;
    FixVector<SpinMutex> locks;

    char padding[64 - sizeof(freelists) - sizeof(segments) - sizeof(locks)];
  };

  static_assert(sizeof(ThreadCache) % 64 == 0);

  bool AllocateSegmentSpace(Segment *segment, uint32_t record_size);

  void init_data_size_2_block_size() {
    data_size_2_block_size_.resize(4096);
    for (size_t i = 0; i < data_size_2_block_size_.size(); i++) {
      data_size_2_block_size_[i] =
          (i / block_size_) + (i % block_size_ == 0 ? 0 : 1);
    }
  }

  inline uint32_t Size2BlockSize(uint32_t data_size) {
    if (data_size < data_size_2_block_size_.size()) {
      return data_size_2_block_size_[data_size];
    }
    return CalculateBlockSize(data_size);
  }

  inline uint32_t CalculateBlockSize(uint32_t data_size) {
    return data_size / block_size_ + (data_size % block_size_ == 0 ? 0 : 1);
  }

  const uint64_t pmem_size_;
  const uint64_t segment_size_;
  const uint32_t block_size_;
  const uint32_t max_classified_record_block_size_;
  const uint32_t bg_thread_interval_;

  char *pmem_;
  SpaceEntryPool pool_;
  std::atomic<uint64_t> segment_head_;
  std::vector<uint32_t> segment_record_size_;

  std::vector<ThreadCache> thread_cache_;
  std::shared_ptr<ThreadManager> thread_manager_;
  std::vector<std::thread> bg_threads_;
  // For quickly get corresponding block size of a requested data size
  std::vector<uint16_t> data_size_2_block_size_;

  bool closing_;
};
