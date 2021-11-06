/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2021 Intel Corporation
 */

#pragma once

#include <stdint.h>
#include <string>

struct PMemAllocatorHint {
  PMemAllocatorHint() : PMemAllocatorHint(1 << 20, 32, 1) {}

  PMemAllocatorHint(uint64_t _segment_size, uint32_t _allocation_unit,
                    uint32_t _bg_thread_interval)
      : segment_size(_segment_size), allocation_unit(_allocation_unit),
        bg_thread_interval(_bg_thread_interval) {
    max_allocation_size = _allocation_unit << 7;
  }

  uint64_t segment_size;
  uint32_t allocation_unit;
  float bg_thread_interval;
  uint64_t max_allocation_size;
};

class PMemAllocator {
public:
  // Allocate a PMem space, return address and actually allocated space in bytes
  virtual void *Allocate(uint64_t size) = 0;

  // Free a PMem space entry. The entry should be allocated by this allocator
  virtual void Free(void *addr) = 0;

  static PMemAllocator *NewPMemAllocator(const std::string &pmem_file,
                                         uint64_t pmem_size,
                                         uint32_t max_access_threads,
                                         bool devdax_mode,
                                         PMemAllocatorHint *hint = nullptr);
};