//  Copyright (c) 2013, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
#pragma once

#ifndef ROCKSDB_LITE

#include <atomic>
#include <limits>
#include <sstream>
#include <string>
#include <vector>
#include <libpmemobj/persistent_ptr.hpp>
#include <libpmemobj/p.hpp>
#include <unordered_map>

#include "rocksdb/cache.h"
#include "utilities/persistent_cache/hash_table.h"
#include "utilities/persistent_cache/hash_table_evictable.h"
#include "utilities/persistent_cache/persistent_cache_tier.h"

// VolatileCacheTier
//
// This file provides persistent cache tier implementation for caching
// key/values in RAM.
//
//        key/values
//           |
//           V
// +-------------------+
// | VolatileCacheTier | Store in an evictable hash table
// +-------------------+
//           |
//           V
//       on eviction
//   pushed to next tier
//
// The implementation is designed to be concurrent. The evictable hash table
// implementation is not concurrent at this point though.
//
// The eviction algorithm is LRU
namespace rocksdb {

template <class T>
struct pLRUElement {
  explicit pLRUElement() : next_(nullptr), prev_(nullptr), refs_(0) {}

  //virtual ~pLRUElement() { assert(!refs_); }

  nvml::obj::persistent_ptr<T> next_;
  nvml::obj::persistent_ptr<T> prev_;
  std::atomic<size_t> refs_;
};

#define MAX_KEY_LEN 100
#define MAX_VAL_LEN 1000
struct root {

};

struct CacheData {

      size_t key_size;
      size_t value_size;
      char key[MAX_KEY_LEN];
      char value[1];
};

class ObjCacheTier : public PersistentCacheTier {
 public:
  explicit ObjCacheTier(std::string path, size_t size,
      const bool is_compressed = true,
      const size_t max_size = std::numeric_limits<size_t>::max());

  virtual ~ObjCacheTier();

  // insert to cache
  Status Insert(const Slice& page_key, const char* data,
                const size_t size) override;
  // lookup key in cache
  Status Lookup(const Slice& page_key, std::unique_ptr<char[]>* data,
                size_t* size) override;

  // is compressed cache ?
  bool IsCompressed() override { return is_compressed_; }

  // erase key from cache
  bool Erase(const Slice& key) override;

  // Expose stats as map
  std::vector<TierStats> Stats() override;

  // Print stats to string
  std::string PrintStats() override;

  uint64_t NewId() override;

 private:
	void rebuild();
  //
  // Cache data abstraction
  //

  //
  // Index and LRU definition
  //
  struct CacheDataHash {
    uint64_t operator()(const CacheData* obj) const {
      assert(obj);
     return std::hash<std::string>()(obj->key);
    }
  };

  struct CacheDataEqual {
    bool operator()(const CacheData* lhs, const CacheData* rhs) const {
      assert(lhs);
      assert(rhs);
      return strcmp(lhs->key, rhs->key) == 0;
    }
  };

  struct Statistics {
    uint64_t cache_misses_ = 0;
    uint64_t cache_hits_ = 0;
    uint64_t cache_inserts_ = 0;
    uint64_t cache_evicts_ = 0;

    double CacheHitPct() const {
      auto lookups = cache_hits_ + cache_misses_;
      return lookups ? 100 * cache_hits_ / static_cast<double>(lookups) : 0.0;
    }

    double CacheMissPct() const {
      auto lookups = cache_hits_ + cache_misses_;
      return lookups ? 100 * cache_misses_ / static_cast<double>(lookups) : 0.0;
    }
  };

  // Evict LRU tail
  bool Evict();

  const bool is_compressed_ = true;    // does it store compressed data
  std::unordered_map<std::string, nvml::obj::persistent_ptr<CacheData>> index;
  std::atomic<uint64_t> max_size_{0};  // Maximum size of the cache
  std::atomic<uint64_t> size_{0};      // Size of the cache
  Statistics stats_;
	uint64_t last_id;

	nvml::obj::pool<root> pop;
};

}  // namespace rocksdb

#endif
