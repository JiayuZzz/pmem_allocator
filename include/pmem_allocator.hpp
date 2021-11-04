/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2021 Intel Corporation
 */

#pragma once

#include <stdint.h>
#include <string>

struct PMemSpaceEntry {
  PMemSpaceEntry() : addr(nullptr), size(0) {}

  PMemSpaceEntry(void *_addr, uint64_t _size) : addr(_addr), size(_size) {}

  void *addr;
  uint64_t size;
};

struct PMemAllocatorHint {
  PMemAllocatorHint() : PMemAllocatorHint(1024, 1 << 20, 32, 1) {}

  PMemAllocatorHint(uint64_t _max_allocation_size, uint64_t _segment_size,
                    uint32_t _allocation_unit, uint32_t _bg_thread_interval)
      : max_allocation_size(_max_allocation_size), segment_size(_segment_size),
        allocation_unit(_allocation_unit),
        bg_thread_interval(_bg_thread_interval) {
    max_common_allocation_size = _allocation_unit << 7;
  }

  uint64_t max_allocation_size;
  uint64_t segment_size;
  uint32_t allocation_unit;
  float bg_thread_interval;
  uint64_t max_common_allocation_size;
};

class PMemAllocator {
public:
  virtual PMemSpaceEntry Allocate(uint64_t size) = 0;

  virtual void Free(const PMemSpaceEntry &entry) = 0;

  static PMemAllocator *NewPMemAllocator(const std::string &pmem_file,
                                         uint64_t pmem_size,
                                         uint32_t max_access_threads,
                                         bool devdax_mode,
                                         PMemAllocatorHint *hint = nullptr);
};