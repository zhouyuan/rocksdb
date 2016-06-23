//  Copyright (c) 2013, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
#ifndef ROCKSDB_LITE

#include "utilities/persistent_cache/pmemobj_tier_impl.h"
#include <libpmemobj/make_persistent_atomic.hpp>
#include <unistd.h>
#include <string>

namespace rocksdb {
ObjCacheTier::ObjCacheTier(std::string path, size_t size,
	const bool is_compressed,const size_t max_size) :
is_compressed_(is_compressed), max_size_(max_size), last_id(0) {

	if (access(path.c_str(), F_OK) != 0) {
	    pop = nvml::obj::pool<root>::create(path, "rocks_cache", size,
	                             S_IRWXU);
	} else {
	    pop = nvml::obj::pool<root>::open(path, "rocks_cache");
	    rebuild();
	}
}

ObjCacheTier::~ObjCacheTier() {

 }

void ObjCacheTier::rebuild() {
	PMEMoid oid;
	POBJ_FOREACH(pop.get_handle(), oid) {
		nvml::obj::persistent_ptr<CacheData> nval = oid;
		std::string k(nval->key, nval->key_size);
		index.insert({k, nval});
	}
}

std::vector<PersistentCacheTier::TierStats> ObjCacheTier::Stats() {
  PersistentCacheTier::TierStats stat;
  stat.insert({"persistent_cache.obj_cache.hits", stats_.cache_hits_});
  stat.insert({"persistent_cache.obj_cache.misses", stats_.cache_misses_});
  stat.insert(
      {"persistent_cacheobj_cache.inserts", stats_.cache_inserts_});
  stat.insert({"persistent_cache.obj_cache.evicts", stats_.cache_evicts_});
  stat.insert(
      {"persistent_cache.obj_cache.hit_pct", stats_.CacheHitPct()});
  stat.insert(
      {"persistent_cache.obj_cache.miss_pct", stats_.CacheMissPct()});

  std::vector<PersistentCacheTier::TierStats> tier_stats;
  if (next_tier()) {
    tier_stats = next_tier()->Stats();
  }
  tier_stats.push_back(stat);
  return tier_stats;
}

std::string ObjCacheTier::PrintStats() {
  std::ostringstream ss;
  ss << "pagecache.objcache.hits: " << stats_.cache_hits_ << std::endl
     << "pagecache.objcache.misses: " << stats_.cache_misses_ << std::endl
     << "pagecache.objcache.inserts: " << stats_.cache_inserts_
     << std::endl
     << "pagecache.objcache.evicts: " << stats_.cache_evicts_ << std::endl
     << "pagecache.objcache.hit_pct: " << stats_.CacheHitPct() << std::endl
     << "pagecache.objcache.miss_pct: " << stats_.CacheMissPct()
     << std::endl
     << PersistentCacheTier::PrintStats();
  return ss.str();
}

struct cachedata_args {
	const char *key;
	const char *value;
	size_t key_len;
	size_t value_len;
};

int cachedata_constr(PMEMobjpool *pop, void *ptr, void *args) {
	CacheData *data = (CacheData *)ptr;
	cachedata_args *carg = (cachedata_args *)args;
	memcpy(data->key, carg->key, carg->key_len);
	memcpy(data->value, carg->value, carg->value_len);
	data->key_size = carg->key_len;
	data->value_size = carg->value_len;

	return 0;
}

Status ObjCacheTier::Insert(const Slice& page_key, const char* data,
                                 const size_t size) {
  // precondition
  assert(data);
  assert(size);

  // increment the size
  size_ += size;

  // check if we have overshot the limit, if so evict some space
  while (size_ > max_size_) {
    if (!Evict()) {
      // unable to evict data, we give up so we don't spike read
      // latency
      assert(size_ >= size);
      size_ -= size;
      return Status::TryAgain("Unable to evict any data");
    }
  }

  assert(size_ >= size);

  // insert order: LRU, followed by index
  nvml::obj::persistent_ptr<CacheData> cache_data;
  cachedata_args args;
  args.key = page_key.data();
  args.key_len = page_key.size();
  args.value = data;
  args.value_len = size;
  pmemobj_alloc(pop.get_handle(), cache_data.raw_ptr(),
  sizeof (CacheData) + args.value_len - 1, 0, cachedata_constr, &args);
  if (cache_data == nullptr) {
	  Evict();
	  return Status::TryAgain("full");
  }

  if (!index.insert({page_key.ToString(), cache_data}).second) {
    // decrement the size that we incremented ahead of time
    assert(size_ >= size);
    size_ -= size;
    // failed to insert to cache, block already in cache
    return Status::TryAgain("key already exists in volatile cache");
  }

  stats_.cache_inserts_++;
  return Status::OK();
}

Status ObjCacheTier::Lookup(const Slice& page_key,
                                 std::unique_ptr<char[]>* result,
                                 size_t* size) {
  nvml::obj::persistent_ptr<CacheData> kv;

  auto ret = index.find(page_key.ToString());
  if (ret != index.end()) {
    // set return data
    size_t vals = ret->second->value_size;
    result->reset(new char[vals]);
    memcpy(result->get(), ret->second->value, vals);
    *size = vals;
    // update stats
    stats_.cache_hits_++;
    return Status::OK();
  }

  stats_.cache_misses_++;

  if (next_tier()) {
    return next_tier()->Lookup(page_key, result, size);
  }

  return Status::NotFound("key not found in volatile cache");
}

bool ObjCacheTier::Erase(const Slice& key) {
  assert(!"not supported");
  return true;
}


std::random_device rd;
std::mt19937 mt(rd());

size_t rand_a_b(size_t a, size_t b)
{
	std::uniform_int_distribution<size_t> dist(a, b);
	return dist(mt);
}


bool ObjCacheTier::Evict()
{
	nvml::obj::persistent_ptr<CacheData> edata = nullptr;
	auto random_it = std::next(std::begin(index),
		rand_a_b(0, index.size() - 1));
	if (random_it == index.end())
		return false;

	index.erase(random_it);
	edata = random_it->second;

	stats_.cache_evicts_++;

	// push the evicted object to the next level
	if (next_tier()) {
	next_tier()->Insert(Slice(edata->key, edata->key_size), edata->value,
		edata->value_size);
	}

	// adjust size and destroy data
	size_ -= edata->value_size;

	nvml::obj::delete_persistent_atomic<CacheData>(edata);

	return true;
}

uint64_t ObjCacheTier::NewId() {
	return __sync_add_and_fetch(&last_id, 1);
}

}  // namespace rocksdb

#endif
