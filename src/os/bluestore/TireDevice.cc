// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2014 Red Hat
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/file.h>

#include "KernelDevice.h"
#include "include/intarith.h"
#include "include/types.h"
#include "include/compat.h"
#include "include/stringify.h"
#include "common/blkdev.h"
#include "common/errno.h"
#if defined(__FreeBSD__)
#include "bsm/audit_errno.h"
#endif
#include "common/debug.h"
#include "common/numa.h"

#include "global/global_context.h"
#include "ceph_io_uring.h"

#define dout_context cct
#define dout_subsys ceph_subsys_bdev
#undef dout_prefix
#define dout_prefix *_dout << "tiredev(" << this << " " << path <<":" << __LINE__<< ") "

TireDevice::TirelDevice(CephContext * cct,
    std::string& block, std::string& cache,
    aio_callback_t cb, void * cbpriv,
    aio_callback_t d_cb, void * d_cbpriv)
  : BlockDevice(cct, cb, cbpriv),
{
    // 一个是异步执行完回调函数
    // 一个是清理函数
}

int TireDevice::open_bdev(const string& p)
{
  bdev = new KernelDevice(cct, cb, cbpriv, d_cb, d_cbpriv);
  return bdev->open(p);
}

int TireDevice::open_cdev(const string& p)
{
  cdev = new KernelDevice(cct, cb, cbpriv, d_cb, d_cbpriv);
  return cdev->open(p);
}

int TireDevice::build_index_tree()
{

  //tree = new Btree();
  // 加载索引信息
  return 0;
}

int TireDevice::open(const string& p)
{
  int r = 0;
  // 格式化的打开

  dout(1) << __func__ << " bdev " << bpath << "cdev" >> cpath << dendl;
  //打开后端设备
  r = open_bdev(p);
 // 如果有缓存设备
  //if ()
  // 打开缓存设备
  r = open_cdev(p);
  // 加载 cache 设置信息 获取cache 范围的索引树
  人= build_indextree();
  return r;
}

int TireDevice::get_devices(std::set<std::string> *ls) const
{
  int r = 0;

  return 0;
}

void TireDevice::close()
{

}

int TireDevice::collect_metadata(const string& prefix, map<string,string> *pm) const
{

  return 0;
}


bool TireDevice::get_thin_utilization(uint64_t *total, uint64_t *avail) const
{

}


int TireDevice::flush()
{
  return 0;
}

void TireDevice::discard_drain()
{

}

void TireDevice::aio_submit(IOContext *ioc)
{

}

int TireDevice::write(
  uint64_t off,
  bufferlist &bl,
  bool buffered,
  int write_hint)
{

}

int TireDevice::aio_write(
  uint64_t off,
  bufferlist &bl,
  IOContext *ioc,
  bool buffered,
  int write_hint)
{
  // 判断模式是不是 writeback
  // 从cache 设置中操作是否有重合范围, 重合就调用缓存设备
  // 否则调用后端设备
  return 0;
}

int TireDevice::discard(uint64_t offset, uint64_t len)
{
  int r = 0;

  return r;
}

int TireDevice::read(uint64_t off, uint64_t len, bufferlist *pbl,
		      IOContext *ioc,
		      bool buffered)
{
  int r = 0;
  // 判断模式是不是 writeback
  // 从cache 设置中操作是否有重合范围
   // 从cache 设置中操作是否有重合范围, 重合就调用缓存设备
  // 否则调用后端设备
  
  return r < 0 ? r : 0;
}

int TireDevice::aio_read(
  uint64_t off,
  uint64_t len,
  bufferlist *pbl,
  IOContext *ioc)
{
  int r = 0;

  // 判断模式是不是 writeback
  // 从cache 设置中操作是否有重合范围
  // 从cache 设置中操作是否有重合范围, 重合就调用缓存设备
  // 否则调用后端设备

  return r;
}

int TireDevice::read_random(uint64_t off, uint64_t len, char *buf,
                       bool buffered)
{
  int r = 0;

  // 判断模式是不是 writeback
  // 从cache 设置中操作是否有重合范围
  // 从cache 设置中操作是否有重合范围, 重合就调用缓存设备
  // 否则调用后端设备

  return r < 0 ? r : 0;
}

int TireDevice::invalidate_cache(uint64_t off, uint64_t len)
{
  int r = 0;

  return r;
}
