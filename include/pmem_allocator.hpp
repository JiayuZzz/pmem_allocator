/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2021 Intel Corporation
 */

#pragma once

#include <stdint.h>
#include <string>

// bg_thread_interval: interval to call bg thread to balance freed space among
// access threads
// allocation_unit: minimal allocation unit, shoud be 2^n and no
// less than 8 bytes
// max_allocation_size: max allocation size of the allocator, recommand no
// larger than allocation_unit * 1024
// segment_size: It should be equal or larger than max(1MB,max_allocation_size),
// recommand larger than 128 * max_allocation_size, it should be devidable by
// allocation_unit
//
// See doc/pmem_allocator.md for more details
struct PMemAllocatorConfig {
  PMemAllocatorConfig() = default;

  PMemAllocatorConfig(uint64_t _segment_size, uint32_t _allocation_unit,
                      uint32_t _bg_thread_interval,
                      uint64_t _max_allocation_size)
      : segment_size(_segment_size), allocation_unit(_allocation_unit),
        bg_thread_interval(_bg_thread_interval),
        max_allocation_size(_max_allocation_size) {}

  uint64_t segment_size = 1 << 20;
  uint32_t allocation_unit = 32;
  float bg_thread_interval = 1.0;
  uint64_t max_allocation_size = 1024;
};

class PMemAllocator {
public:
  // Allocate a PMem space
  virtual void *Allocate(uint64_t size) = 0;

  // Free a PMem space, it should be allocated by this allocator
  virtual void Free(void *addr) = 0;

  // Release this access thread from the allocator, this will be auto-called
  // while the thread exit
  virtual void Release() = 0;

  // Create a new PMem allocator instance, map space at pmem file
  // pmem_file: the file on DAX file system or devdax device for mapping PMem
  // space
  // pmem_size: max usable space max_access_threads: max concurrent
  // threads to access this allocator, resource of a access thread is release
  // only if the thread exit or call allocator->Release()
  // devdax_mode: if set true, use devdax device instead of file system
  // config: allocator internal configs
  //
  // See doc/pmem_allocator.md for more details
  static PMemAllocator *NewPMemAllocator(const std::string &pmem_file,
                                         uint64_t pmem_size,
                                         uint32_t max_access_threads,
                                         bool devdax_mode,
                                         const PMemAllocatorConfig &hint);
};