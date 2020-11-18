// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_OS_BLUESTORE_INDIRECTFREELISTMANAGER_H
#define CEPH_OS_BLUESTORE_INDIRECTFREELISTMANAGER_H

#include <string>
#include <vector>
#include <mutex>
#include <ostream>
#include "kv/KeyValueDB.h"
#include "bluestore_types.h"

class IndirectFreelistManager : FreelistManager {
public:
  CephContext* cct;
  BitmapFreelistManager * bfm;

public:
  IndirectFreelistManager(CephContext* cct) : cct(cct) {}
  ~IndirectFreelistManager() {}

  int create(uint64_t size, uint64_t granularity,
		     KeyValueDB::Transaction txn) override;

  int init(const bluestore_bdev_label_t& l,
    KeyValueDB *kvdb,
    bool db_in_read_only) override;
  void sync(KeyValueDB* kvdb) override;
  void shutdown() override;

  void dump(KeyValueDB *kvdb) override;

  void enumerate_reset() override;
  bool enumerate_next(KeyValueDB *kvdb, uint64_t *offset, uint64_t *length) override;

  void allocate(
    uint64_t offset, uint64_t length,
    KeyValueDB::Transaction txn) override;
  void release(
    uint64_t offset, uint64_t length,
    KeyValueDB::Transaction txn) override;

  inline uint64_t get_size() const override {
    return size;
  }

  inline uint64_t get_alloc_units() const{
    return size / alloc_size;
  }

  inline uint64_t get_alloc_size() const{
    return alloc_size;
  }

  virtual void get_meta(uint64_t target_size,
    std::vector<std::pair<string, string>>* res) const override;
};

#endif
