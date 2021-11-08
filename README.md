# PMem Allocator

## Build

    mkdir build
    cmake .. -DCMAKE_BUILD_TYPE=Release
    make -j

## test

    cd build
    ./allocator_test
    # or bind numa node for best access performance
    numactl --cpunodebind=0 --membind=0 ./allocator_test

## Usage

### API

``` C++
// Create a new PMem allocator instance, map space at pmem file
// pmem_file: the file on DAX file system or devdax device for mapping PMem space
// pmem_size: max usable space
// max_access_threads: max concurrent threads to access this allocator, resource of a access thread is release only if the thread exit or call allocator->Release()
// devdax_mode: if set true, use devdax device instead of file system
// config: allocator internal configs
//
// See doc/pmem_allocator.pdf for more information
static PMemAllocator *PMemAllocator::NewPMemAllocator(const std::string &pmem_file,
                                         uint64_t pmem_size,
                                         uint32_t max_access_threads,
                                         bool devdax_mode,
                                         const PMemAllocatorConfig& config);

// bg_thread_interval: interval to call bg thread to balance freed space among access threads
// allocation_unit: minimal allocation unit, shoud be 2^n and no less than 8 bytes
// max_allocation_size: max allocation size of the allocator, recommand no larger than allocation_unit * 1024
// segment_size: It should be equal or larger than max_allocation_size, recommand larger than 128 * max_allocation_size, it should be devidable by allocation_unit
//
// See doc/pmem_allocator.md for more information
struct PMemAllocatorConfig {
  float bg_thread_interval;
  uint32_t allocation_unit;
  uint64_t max_allocation_size;
  uint64_t segment_size;
};

void* PMemAllocator::Allocate(uint64_t size);
void PMemAllocator::Free(void *addr);
```

### Example

``` C++
#include "pmem_allocator.hpp"

std::string path("/mnt/pmem0/allocator_pool");
PMemAllocatorHint hint;
PMemAllocator* allocator = PMemAllocator::NewPMemAllocator(path, 100ULL << 30, 32, false, hint);
void* ptr = allocator->Allocate(1024);
// do some thin with ptr
allocator->Free(ptr);
```