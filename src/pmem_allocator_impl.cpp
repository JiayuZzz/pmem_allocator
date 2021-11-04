/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2021 Intel Corporation
 */

#include <mutex>
#include <string.h>
#include <thread>
#include <unistd.h>

#include "libpmem.h"
#include "pmem_allocator_impl.hpp"
#include "thread_manager.hpp"

PMemAllocator *PMemAllocator::NewPMemAllocator(const std::string &pmem_file,
                                               uint64_t pmem_size,
                                               uint32_t max_access_threads,
                                               bool devdax_mode,
                                               PMemAllocatorHint *hint) {
  PMemAllocatorHint allocator_configs;
  if (hint != nullptr) {
    allocator_configs = *hint;
  }

  int is_pmem;
  uint64_t mapped_size;
  char *pmem;
  if (!devdax_mode) {
    if ((pmem = (char *)pmem_map_file(pmem_file.c_str(), pmem_size,
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
      fprintf(stderr, "CheckDevDaxAndGetSize %s failed device %s faild: %s\n",
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

    if ((pmem = (char *)mmap(nullptr, pmem_size, flags, MAP_SHARED, fd, 0)) ==
        nullptr) {
      fprintf(stderr, "Mmap devdax device %s faild: %s\n", pmem_file.c_str(),
              strerror(errno));
      return nullptr;
    }
  }

  if (mapped_size != pmem_size) {
    fprintf(stderr, "Pmem map file %s size %lu is not same as expected %lu\n",
            pmem_file.c_str(), mapped_size, pmem_size);
    return nullptr;
  }

  PMemAllocatorImpl *allocator = nullptr;
  try {
    allocator = new PMemAllocatorImpl(pmem, pmem_size, max_access_threads,
                                      allocator_configs);
  } catch (std::bad_alloc &err) {
    fprintf(stderr, "Error while initialize PMemAllocatorImpl: %s\n",
            err.what());
    return nullptr;
  }
  printf("Map pmem space done\n");
  return allocator;
}

void PMemAllocatorImpl::SpaceEntryPool::MoveEntryList(std::vector<void *> &src,
                                                      uint32_t b_size) {
  std::lock_guard<SpinMutex> lg(spins_[b_size]);
  assert(b_size < pool_.size());
  pool_[b_size].emplace_back();
  pool_[b_size].back().swap(src);
}

bool PMemAllocatorImpl::SpaceEntryPool::FetchEntryList(std::vector<void *> &dst,
                                                       uint32_t b_size) {
  std::lock_guard<SpinMutex> lg(spins_[b_size]);
  if (pool_[b_size].size() != 0) {
    dst.swap(pool_[b_size].back());
    pool_[b_size].pop_back();
    return true;
  }
  return false;
}

void PMemAllocatorImpl::BackgroundWork() {
  while (1) {
    if (closing_)
      return;
    usleep(bg_thread_interval_ * 1000000);
    // Move cached list to pool
    std::vector<void *> moving_list;
    for (auto &tc : thread_cache_) {
      moving_list.clear();
      for (size_t b_size = 1; b_size < tc.freelists.size(); b_size++) {
        moving_list.clear();
        std::unique_lock<SpinMutex> ul(tc.locks[b_size]);

        if (tc.freelists[b_size].size() >= kMinMovableListSize) {
          if (tc.freelists[b_size].size() >= kMinMovableListSize) {
            moving_list.swap(tc.freelists[b_size]);
          }
        }
        if (moving_list.size() > 0) {
          pool_.MoveEntryList(moving_list, b_size);
        }
      }
    }
  }
}

PMemAllocatorImpl::PMemAllocatorImpl(char *pmem, uint64_t pmem_size,
                                     uint32_t max_access_threads,
                                     const PMemAllocatorHint &hint)
    : pmem_(pmem), pmem_size_(pmem_size),
      thread_manager_(std::make_shared<ThreadManager>(max_access_threads)),
      block_size_(hint.allocation_unit), segment_size_(hint.segment_size),
      bg_thread_interval_(hint.bg_thread_interval),
      max_classified_record_block_size_(
          calculate_block_size(hint.max_common_allocation_size)),
      pool_(max_classified_record_block_size_),
      thread_cache_(max_access_threads, max_classified_record_block_size_),
      offset_head_(0), closing_(false) {
  init_data_size_2_block_size();
  if (bg_thread_interval_ > 0) {
    bg_threads_.emplace_back(&PMemAllocatorImpl::BackgroundWork, this);
  }
}

void PMemAllocatorImpl::Free(const PMemSpaceEntry &entry) {
  if (!MaybeInitAccessThread()) {
    fprintf(stderr, "too many thread access allocator!\n");
    std::abort();
  }

  if (entry.size > 0 && entry.addr != nullptr) {
    assert(entry.size % block_size_ == 0);
    auto b_size = entry.size / block_size_;
    auto &thread_cache = thread_cache_[access_thread.id];
    std::unique_lock<SpinMutex> ul(thread_cache.locks[b_size]);
    assert(b_size < thread_cache.freelists.size());
    // Conflict with bg thread happens only if free entries more than
    // kMinMovableListSize
    thread_cache.freelists[b_size].emplace_back(entry.addr);
  }
}

void PMemAllocatorImpl::PopulateSpace() {
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
  for (auto &t : ths) {
    t.join();
  }
  printf("Populating done\n");
}

PMemAllocatorImpl::~PMemAllocatorImpl() {
  closing_ = true;
  for (auto &t : bg_threads_) {
    t.join();
  }
  pmem_unmap(pmem_, pmem_size_);
}

bool PMemAllocatorImpl::AllocateSegmentSpace(PMemSpaceEntry *segment_entry) {
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

PMemSpaceEntry PMemAllocatorImpl::Allocate(uint64_t size) {
  PMemSpaceEntry space_entry;
  if (!MaybeInitAccessThread()) {
    fprintf(stderr, "too many thread access allocator!\n");
    return space_entry;
  }
  uint32_t b_size = size_2_block_size(size);
  uint32_t aligned_size = b_size * block_size_;
  // Now the requested block size should smaller than segment size
  if (aligned_size > segment_size_ || aligned_size == 0) {
    fprintf(stderr,
            "allocating size is 0 or larger than PMem allocator segment\n");
    return space_entry;
  }
  auto &thread_cache = thread_cache_[access_thread.id];
  for (auto i = b_size; i < thread_cache.freelists.size(); i++) {
    if (thread_cache.segments[i].size < aligned_size) {
      // Fetch free list from pool
      {
        std::unique_lock<SpinMutex> ul(thread_cache.locks[i]);
        if (thread_cache.freelists[i].empty()) {
          pool_.FetchEntryList(thread_cache.freelists[i], i);
        }
        // Get space from free list
        if (thread_cache.freelists[i].size() > 0) {
          space_entry.addr = thread_cache.freelists[i].back();
          space_entry.size = i * block_size_;
          thread_cache.freelists[i].pop_back();
          break;
        }
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
    thread_cache.segments[i].addr =
        (char *)thread_cache.segments[i].addr + aligned_size;
    break;
  }
  return space_entry;
}
