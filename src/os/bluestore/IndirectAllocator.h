// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */
#ifndef CEPH_OS_BLUESTORE_INDERECTALLOCATOR_H
#define CEPH_OS_BLUESTORE_INDERECTALLOCATOR_H

#include <ostream>
#include "include/ceph_assert.h"
#include "os/bluestore/bluestore_types.h"
#include <functional>

class InderectAllocator :Allocator {
public:
  int64_t capacity,
  int64_t alloc_unit,
public:
  explicit InderectAllocator(const std::string& name);
  ~InderectAllocator() final;

  /*
   * Allocate required number of blocks in n number of extents.
   * Min and Max number of extents are limited by:
   * a. alloc unit
   * b. max_alloc_size.
   * as no extent can be lesser than alloc_unit and greater than max_alloc size.
   * Apart from that extents can vary between these lower and higher limits according
   * to free block search algorithm and availability of contiguous space.
   */
  virtual int64_t allocate(uint64_t want_size, uint64_t alloc_unit,
			   uint64_t max_alloc_size, int64_t hint,
			   PExtentVector *extents) override;

  /* Bulk release. Implementations may override this method to handle the whole
   * set at once. This could save e.g. unnecessary mutex dance. */
  virtual void release(const interval_set<uint64_t>& release_set) override;

  virtual void dump() override;
  virtual void dump(std::function<void(uint64_t offset, uint64_t length)> notify) override;

  virtual void init_add_free(uint64_t offset, uint64_t length) override;
  virtual void init_rm_free(uint64_t offset, uint64_t length) override;

  virtual uint64_t get_free() override;
  virtual double get_fragmentation() override;
  virtual double get_fragmentation_score() override;
  virtual void shutdown() override;

private:
  class SocketHook;
  SocketHook* asok_hook = nullptr;
};

#endif
