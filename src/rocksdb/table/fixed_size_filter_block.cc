// Copyright (c) YugaByte, Inc.
//
// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <algorithm>

#include "db/dbformat.h"
#include "rocksdb/filter_policy.h"
#include "util/coding.h"
#include "util/perf_context_imp.h"
#include "util/string_util.h"
#include "table/fixed_size_filter_block.h"

namespace rocksdb {

namespace {

void AppendItem(std::string* props, const std::string& key,
                const std::string& value) {
  char cspace = ' ';
  std::string value_str("");
  size_t i = 0;
  const size_t dataLength = 64;
  const size_t tabLength = 2;
  const size_t offLength = 16;

  value_str.append(&value[i], std::min(size_t(dataLength), value.size()));
  i += dataLength;
  while (i < value.size()) {
    value_str.append("\n");
    value_str.append(offLength, cspace);
    value_str.append(&value[i], std::min(size_t(dataLength), value.size() - i));
    i += dataLength;
  }

  std::string result("");
  if (key.size() < (offLength - tabLength))
    result.append(size_t((offLength - tabLength)) - key.size(), cspace);
  result.append(key);

  props->append(result + ": " + value_str + "\n");
}

template <class TKey>
void AppendItem(std::string* props, const TKey& key, const std::string& value) {
  std::string key_str = rocksdb::ToString(key);
  AppendItem(props, key_str, value);
}
}  // namespace


// See doc/table_format.txt for an explanation of the filter block format.

// Generate new filter for fixed number of keys
FixedSizeFilterBlockBuilder::FixedSizeFilterBlockBuilder(
    const SliceTransform* prefix_extractor,
    const BlockBasedTableOptions& table_opt)
    : policy_(table_opt.filter_policy.get()),
      prefix_extractor_(prefix_extractor),
      whole_key_filtering_(table_opt.whole_key_filtering),
      bits_builder_(nullptr) {
}

void FixedSizeFilterBlockBuilder::StartBlock(uint64_t block_offset) {
  assert(policy_);
  assert(!bits_builder_);
  FilterBitsBuilder* filter_bits_builder_ = policy_->GetFilterBitsBuilder();
  assert(filter_bits_builder_);
  bits_builder_.reset(filter_bits_builder_);
}

void FixedSizeFilterBlockBuilder::Add(const Slice& key) {
  if (prefix_extractor_ && prefix_extractor_->InDomain(key)) {
    AddPrefix(key);
  }

  if (whole_key_filtering_) {
    AddKey(key);
  }
}

void FixedSizeFilterBlockBuilder::AddKey(const Slice& key) {
  assert(bits_builder_);
  bits_builder_->AddKey(key);
}

void FixedSizeFilterBlockBuilder::AddPrefix(const Slice& key) {
  assert(bits_builder_);
  Slice prefix = prefix_extractor_->Transform(key);
  bits_builder_->AddKey(prefix);
}

bool FixedSizeFilterBlockBuilder::ShouldFlush() const {
  assert(bits_builder_);
  return bits_builder_->IsFull();
}

Slice FixedSizeFilterBlockBuilder::Finish() {
  assert(bits_builder_);
  Slice finished_slice = bits_builder_->Finish(&active_block_data_);
  bits_builder_.reset();
  results_.push_back(std::move(active_block_data_));
  return finished_slice;
}

FixedSizeFilterBlockReader::FixedSizeFilterBlockReader(
    const SliceTransform* prefix_extractor,
    const BlockBasedTableOptions& table_opt,
    bool whole_key_filtering,
    BlockContents&& contents)
    : policy_(table_opt.filter_policy.get()),
      prefix_extractor_(prefix_extractor),
      whole_key_filtering_(whole_key_filtering),
      contents_(std::move(contents)) {
  assert(policy_);
  reader_.reset(policy_->GetFilterBitsReader(contents_.data));
}

bool FixedSizeFilterBlockReader::KeyMayMatch(const Slice& key, uint64_t block_offset) {
  return whole_key_filtering_ ? MayMatch(key) : true;
}

bool FixedSizeFilterBlockReader::PrefixMayMatch(const Slice& prefix, uint64_t block_offset) {
  return prefix_extractor_ ? MayMatch(prefix) : true;
}

bool FixedSizeFilterBlockReader::MayMatch(const Slice& entry) {
  if (reader_->MayMatch(entry)) {
    PERF_COUNTER_ADD(bloom_sst_hit_count, 1);
    return true;
  } else {
    PERF_COUNTER_ADD(bloom_sst_miss_count, 1);
    return false;
  }
}

size_t FixedSizeFilterBlockReader::ApproximateMemoryUsage() const {
  return contents_.data.size_;
}

std::string FixedSizeFilterBlockReader::ToString() const {
  std::string result;
  result.reserve(contents_.data.size_ * 2);
  Slice filter = Slice(contents_.data.data_, contents_.data.size_);
  AppendItem(&result, std::string("Filter block"), filter.ToString(true));
  return result;
}
}  // namespace rocksdb