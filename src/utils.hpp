/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2021 Intel Corporation
 */

#pragma once

#include <atomic>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#define PATH_MAX 255

template <typename T> class FixVector {
public:
  FixVector(uint64_t size) : size_(size) { data_ = new T[size]; }

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

  uint64_t size() { return size_; }

private:
  T *data_;
  uint64_t size_;
};

class SpinMutex {
private:
  std::atomic_flag locked = ATOMIC_FLAG_INIT;
  //  int owner = -1;

public:
  void lock() {
    while (locked.test_and_set(std::memory_order_acquire)) {
      asm volatile("pause");
    }
    //    owner = access_thread.id;
  }

  void unlock() {
    //    owner = -1;
    locked.clear(std::memory_order_release);
  }

  bool try_lock() {
    if (locked.test_and_set(std::memory_order_acquire)) {
      return false;
    }
    //    owner = access_thread.id;
    return true;
  }

  //  bool hold() { return owner == access_thread.id; }

  SpinMutex(const SpinMutex &s) : locked(ATOMIC_FLAG_INIT) {}

  SpinMutex(const SpinMutex &&s) : locked(ATOMIC_FLAG_INIT) {}

  SpinMutex() : locked(ATOMIC_FLAG_INIT) {}
};

static bool CheckDevDaxAndGetSize(const char *path, uint64_t *size) {
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
    fprintf(stderr, "realpath on file %s failed %s\n", spath, strerror(errno));
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