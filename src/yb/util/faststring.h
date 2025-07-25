// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
#pragma once

#include <string>

#include "yb/gutil/dynamic_annotations.h"
#include "yb/gutil/macros.h"
#include "yb/gutil/strings/fastmem.h"

namespace yb {

// A faststring is similar to a std::string, except that it is faster for many
// common use cases (in particular, resize() will fill with uninitialized data
// instead of memsetting to \0)
class faststring {
 public:
  // Tell PVS Studio it is OK that initial_data_ is uninitialized.
  //-V:initial_data_:730
  faststring()
      : data_(initial_data_),
        len_(0),
        capacity_(kInitialCapacity) {
  }

  // Construct a string with the given capacity, in bytes.
  // Tell PVS Studio it is OK that initial_data_ is uninitialized.
  //-V:initial_data_:730
  explicit faststring(size_t capacity)
      : data_(initial_data_),
        len_(0),
        capacity_(kInitialCapacity) {
    if (capacity > capacity_) {
      data_ = new uint8_t[capacity];
      capacity_ = capacity;
    }
    ASAN_POISON_MEMORY_REGION(data_, capacity_);
  }

  ~faststring() {
    ASAN_UNPOISON_MEMORY_REGION(initial_data_, arraysize(initial_data_));
    if (data_ != initial_data_) {
      delete[] data_;
    }
  }

  // Reset the valid length of the string to 0.
  //
  // This does not free up any memory. The capacity of the string remains unchanged.
  void clear() {
    resize(0);
    ASAN_POISON_MEMORY_REGION(data_, capacity_);
  }

  // Resize the string to the given length.
  // If the new length is larger than the old length, the capacity is expanded as necessary.
  //
  // NOTE: in contrast to std::string's implementation, Any newly "exposed" bytes of data are
  // not cleared.
  void resize(size_t newsize) {
    if (newsize > capacity_) {
      reserve(newsize);
    }
    len_ = newsize;
    ASAN_POISON_MEMORY_REGION(data_ + len_, capacity_ - len_);
    ASAN_UNPOISON_MEMORY_REGION(data_, len_);
  }

  // Releases the underlying array; after this, the buffer is left empty.
  //
  // NOTE: the data pointer returned by release() is not necessarily the pointer
  uint8_t *release() WARN_UNUSED_RESULT {
    uint8_t *ret = data_;
    if (ret == initial_data_) {
      ret = new uint8_t[len_];
      memcpy(ret, data_, len_);
    }
    len_ = 0;
    capacity_ = kInitialCapacity;
    data_ = initial_data_;
    ASAN_POISON_MEMORY_REGION(data_, capacity_);
    return ret;
  }

  // Reserve space for the given total amount of data. If the current capacity is already
  // larger than the newly requested capacity, this is a no-op (i.e. it does not ever free memory).
  //
  // NOTE: even though the new capacity is reserved, it is illegal to begin writing into that memory
  // directly using pointers. If ASAN is enabled, this is ensured using manual memory poisoning.
  void reserve(size_t newcapacity) {
    if (PREDICT_TRUE(newcapacity <= capacity_)) return;
    GrowArray(newcapacity);
  }

  // Append the given data to the string, resizing capacity as necessary.
  void append(const void *src_v, size_t count) {
    const uint8_t *src = reinterpret_cast<const uint8_t *>(src_v);
    EnsureRoomForAppend(count);
    ASAN_UNPOISON_MEMORY_REGION(data_ + len_, count);

    // appending short values is common enough that this
    // actually helps, according to benchmarks. In theory
    // memcpy_inlined should already be just as good, but this
    // was ~20% faster for reading a large prefix-coded string file
    // where each string was only a few chars different
    if (count <= 4) {
      uint8_t *p = &data_[len_];
      for (size_t i = 0; i < count; i++) {
        *p++ = *src++;
      }
    } else {
      strings::memcpy_inlined(&data_[len_], src, count);
    }
    len_ += count;
  }

  // Append the given string to this string.
  void append(const std::string &str) {
    append(str.data(), str.size());
  }

  // Append the given character to this string.
  void push_back(const char byte) {
    EnsureRoomForAppend(1);
    ASAN_UNPOISON_MEMORY_REGION(data_ + len_, 1);
    data_[len_] = byte;
    len_++;
  }

  // Return true if the string is empty.
  bool empty() const {
    return len_ == 0;
  }

  // Return the valid length of this string.
  size_t length() const {
    return len_;
  }

  // Return the valid length of this string (identical to length())
  size_t size() const {
    return len_;
  }

  // Return the allocated capacity of this string.
  size_t capacity() const {
    return capacity_;
  }

  // Return a pointer to the data in this string. Note that this pointer
  // may be invalidated by any later non-const operation.
  const uint8_t *data() const {
    return &data_[0];
  }

  // Return a pointer to the data in this string. Note that this pointer
  // may be invalidated by any later non-const operation.
  //
  // WARNING: This used to be called c_str(), but does not meet the specification of that method:
  // this method does not return a null-terminated string!  Accordingly it has been renamed.
  const char *char_data() const {
    return reinterpret_cast<const char *>(data());
  }

  // Return a pointer to the data in this string. Note that this pointer
  // may be invalidated by any later non-const operation.
  uint8_t *data() {
    return &data_[0];
  }

  // Return the given element of this string. Note that this does not perform
  // any bounds checking.
  const uint8_t &at(size_t i) const {
    return data_[i];
  }

  // Return the given element of this string. Note that this does not perform
  // any bounds checking.
  const uint8_t &operator[](size_t i) const {
    return data_[i];
  }

  // Return the given element of this string. Note that this does not perform
  // any bounds checking.
  uint8_t &operator[](size_t i) {
    return data_[i];
  }

  // Reset the contents of this string by copying 'len' bytes from 'src'.
  void assign_copy(const uint8_t *src, size_t len) {
    // Reset length so that the first resize doesn't need to copy the current
    // contents of the array.
    len_ = 0;
    resize(len);
    memcpy(data(), src, len);
  }

  // Reset the contents of this string by copying from the given std::string.
  void assign_copy(const std::string &str) {
    assign_copy(reinterpret_cast<const uint8_t *>(str.c_str()),
                str.size());
  }

  void CopyFrom(const faststring& other) {
    clear();
    append(other.data(), other.size());
  }

  // Return a copy of this string as a std::string.
  std::string ToString() const {
    return std::string(reinterpret_cast<const char *>(data()),
                       len_);
  }

 private:

  // If necessary, expand the buffer to fit at least 'count' more bytes.
  // If the array has to be grown, it is grown by at least 50%.
  void EnsureRoomForAppend(size_t count) {
    if (PREDICT_TRUE(len_ + count <= capacity_)) {
      return;
    }

    // Call the non-inline slow path - this reduces the number of instructions
    // on the hot path.
    GrowByAtLeast(count);
  }

  // The slow path of MakeRoomFor. Grows the buffer by either
  // 'count' bytes, or 50%, whichever is more.
  void GrowByAtLeast(size_t count);

  // Grow the array to the given capacity, which must be more than
  // the current capacity.
  void GrowArray(size_t newcapacity);

  enum {
    kInitialCapacity = 32
  };

  uint8_t* data_;
  uint8_t initial_data_[kInitialCapacity];
  size_t len_;
  size_t capacity_;

  DISALLOW_COPY_AND_ASSIGN(faststring);
};

}  // namespace yb
