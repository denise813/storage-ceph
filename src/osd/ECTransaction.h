// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2013 Inktank Storage, Inc.
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef ECTRANSACTION_H
#define ECTRANSACTION_H

#include "OSD.h"
#include "PGBackend.h"
#include "ECUtil.h"
#include "erasure-code/ErasureCodeInterface.h"
#include "PGTransaction.h"
#include "ExtentCache.h"

namespace ECTransaction {
  struct WritePlan {
    PGTransactionUPtr t;
    bool invalidates_cache = false; // Yes, both are possible
    map<hobject_t,extent_set> to_read;
    map<hobject_t,extent_set> will_write; // superset of to_read

    map<hobject_t,ECUtil::HashInfoRef> hash_infos;
  };

  bool requires_overwrite(
    uint64_t prev_size,
    const PGTransaction::ObjectOperation &op);

  template <typename F>
  WritePlan get_write_plan(
    const ECUtil::stripe_info_t &sinfo,
    PGTransactionUPtr &&t,
    F &&get_hinfo,
    DoutPrefixProvider *dpp) {
    WritePlan plan;
/** comment by hy 2020-09-19
 * # 转换为object 事务
 */
    t->safe_create_traverse(
      [&](pair<const hobject_t, PGTransaction::ObjectOperation> &i) {
/** comment by hy 2020-10-21
 * # i = pg log 中信息
 */
/** comment by hy 2020-10-21
 * # get_hinfo 回调函数，获取信息
 */
	ECUtil::HashInfoRef hinfo = get_hinfo(i.first);
/** comment by hy 2020-10-21
 * # 标记 i 信息
 */
	plan.hash_infos[i.first] = hinfo;

/** comment by hy 2020-10-22
 * # 实际未来执行对齐后的逻辑长度
 */
	uint64_t projected_size =
	  hinfo->get_projected_total_logical_size(sinfo);

	if (i.second.deletes_first()) {
	  ldpp_dout(dpp, 20) << __func__ << ": delete, setting projected size"
			     << " to 0" << dendl;
	  projected_size = 0;
	}

	hobject_t source;
	if (i.second.has_source(&source)) {
	  plan.invalidates_cache = true;

/** comment by hy 2020-10-21
 * # 获取操作源信息
 */
	  ECUtil::HashInfoRef shinfo = get_hinfo(source);
	  projected_size = shinfo->get_projected_total_logical_size(sinfo);
	  plan.hash_infos[source] = shinfo;
	}

/** comment by hy 2020-10-21
 * # 
 */
	auto &will_write = plan.will_write[i.first];
	if (i.second.truncate &&
	    i.second.truncate->first < projected_size) {
/** comment by hy 2020-10-21
 * # 非条带对齐
 */
	  if (!(sinfo.logical_offset_is_stripe_aligned(
		  i.second.truncate->first))) {
/** comment by hy 2020-10-21
 * # 设置的读取的值,进行范围对齐
 */
	    plan.to_read[i.first].union_insert(
	      sinfo.logical_to_prev_stripe_offset(i.second.truncate->first),
	      sinfo.get_stripe_width());

	    ldpp_dout(dpp, 20) << __func__ << ": unaligned truncate" << dendl;
/** comment by hy 2020-10-22
 * # 设置写入的值
 */
	    will_write.union_insert(
	      sinfo.logical_to_prev_stripe_offset(i.second.truncate->first),
	      sinfo.get_stripe_width());
	  }
	  projected_size = sinfo.logical_to_next_stripe_offset(
	    i.second.truncate->first);
	}

/** comment by hy 2020-10-22
 * # 原来的正确范围
 */
	extent_set raw_write_set;
	for (auto &&extent: i.second.buffer_updates) {
	  using BufferUpdate = PGTransaction::ObjectOperation::BufferUpdate;
	  if (boost::get<BufferUpdate::CloneRange>(&(extent.get_val()))) {
	    ceph_assert(
	      0 ==
	      "CloneRange is not allowed, do_op should have returned ENOTSUPP");
	  }
	  raw_write_set.insert(extent.get_off(), extent.get_len());
	}

/** comment by hy 2020-10-22
 * # 
 */
	auto orig_size = projected_size;
	for (auto extent = raw_write_set.begin();
	     extent != raw_write_set.end();
	     ++extent) {
	  uint64_t head_start =
	    sinfo.logical_to_prev_stripe_offset(extent.get_start());
	  uint64_t head_finish =
	    sinfo.logical_to_next_stripe_offset(extent.get_start());
	  if (head_start > projected_size) {
	    head_start = projected_size;
	  }
	  if (head_start != head_finish &&
	      head_start < orig_size) {
	    ceph_assert(head_finish <= orig_size);
	    ceph_assert(head_finish - head_start == sinfo.get_stripe_width());
	    ldpp_dout(dpp, 20) << __func__ << ": reading partial head stripe "
			       << head_start << "~" << sinfo.get_stripe_width()
			       << dendl;
	    plan.to_read[i.first].union_insert(
	      head_start, sinfo.get_stripe_width());
	  }

	  uint64_t tail_start =
	    sinfo.logical_to_prev_stripe_offset(
	      extent.get_start() + extent.get_len());
	  uint64_t tail_finish =
	    sinfo.logical_to_next_stripe_offset(
	      extent.get_start() + extent.get_len());
	  if (tail_start != tail_finish &&
	      (head_start == head_finish || tail_start != head_start) &&
	      tail_start < orig_size) {
	    ceph_assert(tail_finish <= orig_size);
	    ceph_assert(tail_finish - tail_start == sinfo.get_stripe_width());
	    ldpp_dout(dpp, 20) << __func__ << ": reading partial tail stripe "
			       << tail_start << "~" << sinfo.get_stripe_width()
			       << dendl;
	    plan.to_read[i.first].union_insert(
	      tail_start, sinfo.get_stripe_width());
	  }

	  if (head_start != tail_finish) {
	    ceph_assert(
	      sinfo.logical_offset_is_stripe_aligned(
		tail_finish - head_start)
	      );
	    will_write.union_insert(
	      head_start, tail_finish - head_start);
	    if (tail_finish > projected_size)
	      projected_size = tail_finish;
	  } else {
	    ceph_assert(tail_finish <= projected_size);
	  }
	}

	if (i.second.truncate &&
	    i.second.truncate->second > projected_size) {
	  uint64_t truncating_to =
	    sinfo.logical_to_next_stripe_offset(i.second.truncate->second);
	  ldpp_dout(dpp, 20) << __func__ << ": truncating out to "
			     <<  truncating_to
			     << dendl;
	  will_write.union_insert(projected_size,
				  truncating_to - projected_size);
	  projected_size = truncating_to;
	}

	ldpp_dout(dpp, 20) << __func__ << ": " << i.first
			   << " projected size "
			   << projected_size
			   << dendl;
	hinfo->set_projected_total_logical_size(
	  sinfo,
	  projected_size);

	/* validate post conditions:
	 * to_read should have an entry for i.first iff it isn't empty
	 * and if we are reading from i.first, we can't be renaming or
	 * cloning it */
	ceph_assert(plan.to_read.count(i.first) == 0 ||
	       (!plan.to_read.at(i.first).empty() &&
		!i.second.has_source()));
      });
    plan.t = std::move(t);
    return plan;
  }

  void generate_transactions(
    WritePlan &plan,
    ErasureCodeInterfaceRef &ecimpl,
    pg_t pgid,
    const ECUtil::stripe_info_t &sinfo,
    const map<hobject_t,extent_map> &partial_extents,
    vector<pg_log_entry_t> &entries,
    map<hobject_t,extent_map> *written,
    map<shard_id_t, ObjectStore::Transaction> *transactions,
    set<hobject_t> *temp_added,
    set<hobject_t> *temp_removed,
    DoutPrefixProvider *dpp,
    const ceph_release_t require_osd_release = ceph_release_t::unknown);
};

#endif
