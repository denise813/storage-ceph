// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_OS_BLUESTORE_BITMAPFASTALLOCATOR_H
#define CEPH_OS_BLUESTORE_BITMAPFASTALLOCATOR_H

#include <mutex>

#include "Allocator.h"
#include "os/bluestore/bluestore_types.h"
#include "fastbmap_allocator_impl.h"
#include "include/mempool.h"
#include "common/debug.h"

class BitmapAllocator : public Allocator,
  public AllocatorLevel02<AllocatorLevel01Loose> {
  CephContext* cct;

public:
  BitmapAllocator(CephContext* _cct, int64_t capacity, int64_t alloc_unit, const std::string& name);
  ~BitmapAllocator() override
  {
  }

/** comment by hy 2020-02-27
 * # 分配空间,分配空间不一定连续,有可能是离散的
     如果同时指定 hint 参数,用于对下次开始分配的起始地址
     进行预测
 */
  int64_t allocate(
    uint64_t want_size, uint64_t alloc_unit, uint64_t max_alloc_size,
    int64_t hint, PExtentVector *extents) override;

/** comment by hy 2020-02-27
 * # 释放空间
 */
  void release(
    const interval_set<uint64_t>& release_set) override;

  using Allocator::release;

/** comment by hy 2020-02-27
 * # 返回所有空闲空间大小
 */
  uint64_t get_free() override
  {
    return get_available();
  }

  void dump() override;
  void dump(std::function<void(uint64_t offset, uint64_t length)> notify) override;
  double get_fragmentation() override
  {
    return _get_fragmentation();
  }

/** comment by hy 2020-02-27
 * # 读写磁盘中空闲的段,标记相应的段空间为空闲
 */
  void init_add_free(uint64_t offset, uint64_t length) override;
/** comment by hy 2020-02-27
 * # 指定范围的空间标记为已分配
 */
  void init_rm_free(uint64_t offset, uint64_t length) override;

  void shutdown() override;
};

#endif
