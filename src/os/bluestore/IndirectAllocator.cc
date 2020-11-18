// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "Allocator.h"
#include "StupidAllocator.h"
#include "BitmapAllocator.h"
#include "AvlAllocator.h"
#include "HybridAllocator.h"
#include "common/debug.h"
#include "common/admin_socket.h"
#define dout_subsys ceph_subsys_bluestore

InderectAllocator::InderectAllocator(CephContext* _cct,
                                         int64_t capacity,
                                         const std::string& name) :
    Allocator(name),
    cct(_cct)
{
  ldout(cct, 10) << __func__ << " 0x" << std::hex << capacity << "/"
                 << alloc_unit << std::dec << dendl;
  // 一次向磁盘分配的最小单元 block
  // 获取可以分配的zone 信息
}

InderectAllocator::~InderectAllocator() {}

int64_t InderectAllocator::allocate(
  uint64_t want_size,
  uint64_t alloc_unit,
  uint64_t max_alloc_size,
  int64_t hint,
  PExtentVector *extents) {
   uint64_t current_zone = current_zone_num;

   for (int i =0; i< zone_num; i++) {
     //判断 后续zone 是否有空间 ((current_zone % zone_num) + i);
     //如果有记使用空间,将 current 记录为当前范围
     //如果都没有将返回无空间
   }

   //从当前zone 获取空间现在的偏移位置
   
  return 0;
}

void InderectAllocator::release(const interval_set<uint64_t>& release_set) {
  // 真释放 release_set 是一个map 结构 里面的每个元素包含长度和范围
  // 判断是不是需要进行强制删除
  // 如果要进行删除就将其他的移动到其他的
  // 容器中
}

uint64_t InderectAllocator::get_free() {
  return num_free;
}

void InderectAllocator::dump() {
}

void InderectAllocator::dump(std::function<void(uint64_t offset,
					     uint64_t length)> notify) {
  // 查看分配情况
}

// This just increments |num_free|.  The actual free space is added by
// set_zone_states, as it updates the write pointer for each zone.
void InderectAllocator::init_add_free(uint64_t offset, uint64_t length) {
  // 标记为空闲
}

void InderectAllocator::init_rm_free(uint64_t offset, uint64_t length) {
  // 标记为使用
  
}

void InderectAllocator::shutdown() {
  
}


