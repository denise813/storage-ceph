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

#ifndef CEPH_OS_BLUESTORE_TIREDEVICE_H
#define CEPH_OS_BLUESTORE_TIREDEVICE_H

#include <atomic>

#include "include/types.h"
#include "include/interval_set.h"
#include "common/Thread.h"
#include "include/utime.h"

#include "ceph_aio.h"
#include "BlockDevice.h"

#define RW_IO_MAX (INT_MAX & CEPH_PAGE_MASK)


class TireDevice : public BlockDevice {
private:
  std::string devname;

private:
  // 基类里面的 size 只能是存放数据的大小
  // 因为 get_size 就是取这个值
  BlockDevice * bdev = nullptr;
  BlockDevice * cdev = nullptr;
  CephContext* cct;
  aio_callback_t cb;
  void *cbpriv;
  aio_callback_t d_cb;
  void *d_cbpriv;

#if 0
  struct AioCompletionThread : public Thread {
/** comment by hy 2020-04-22
 * # Libaio线程 收割完成的事件
 */
    KernelDevice *bdev;
    explicit AioCompletionThread(KernelDevice *b) : bdev(b) {}
    void *entry() override {
      bdev->_aio_thread();
      return NULL;
    }
  } aio_thread;

  struct DiscardThread : public Thread {
/** comment by hy 2020-04-22
 * # SSD的Trim
 */
    KernelDevice *bdev;
    explicit DiscardThread(KernelDevice *b) : bdev(b) {}
    void *entry() override {
      bdev->_discard_thread();
      return NULL;
    }
  } discard_thread;
#endif

  // 一个回刷线程
  struct AioFlushThread : public Thread {
  };

public:
  TireDevice(CephContext * cct,
    std::string& block, std::string& cache,
    aio_callback_t cb, void * cbpriv,
    aio_callback_t d_cb, void * d_cbpriv);

private:
  int open_bdev(const std::string& path);
  int open_cdev(const std::string& path);

public:
  void aio_submit(IOContext *ioc) override;
  void discard_drain() override;

  int collect_metadata(const std::string& prefix, map<std::string,std::string> *pm) const override;
  int get_devname(std::string *s) const override {
    if (devname.empty()) {
      return -ENOENT;
    }
    *s = devname;
    return 0;
  }
  int get_devices(std::set<std::string> *ls) const override;

  bool get_thin_utilization(uint64_t *total, uint64_t *avail) const override;
/** comment by hy 2020-04-22
 * # 同步读接口
 */
  int read(uint64_t off, uint64_t len, bufferlist *pbl,
	   IOContext *ioc,
	   bool buffered) override;
/** comment by hy 2020-04-22
 * # 异步读接口
 */
  int aio_read(uint64_t off, uint64_t len, bufferlist *pbl,
	       IOContext *ioc) override;
  int read_random(uint64_t off, uint64_t len, char *buf, bool buffered) override;

  int write(uint64_t off, bufferlist& bl, bool buffered, int write_hint = WRITE_LIFE_NOT_SET) override;
  int aio_write(uint64_t off, bufferlist& bl,
		IOContext *ioc,
		bool buffered,
		int write_hint = WRITE_LIFE_NOT_SET) override;
  int flush() override;
/** comment by hy 2020-04-22
 * # SSD指定offset、len的数据做Trim
 */
  int discard(uint64_t offset, uint64_t len) override;

  // for managing buffered readers/writers
  int invalidate_cache(uint64_t off, uint64_t len) override;
  int open(const std::string& path) override;
  void close() override;
};

#endif
