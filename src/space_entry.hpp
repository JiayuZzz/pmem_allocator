#pragma once
#include <stdint.h>

struct PMemSpaceEntry {
  PMemSpaceEntry() : addr(nullptr), size(0) {}
  PMemSpaceEntry(void *_addr, uint64_t _size) : addr(_addr), size(_size) {}

  void *addr;
  uint64_t size;
};