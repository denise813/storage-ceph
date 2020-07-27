// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2012 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */
#ifndef CEPH_LIBRADOS_RADOSCLIENT_H
#define CEPH_LIBRADOS_RADOSCLIENT_H

#include "common/config_fwd.h"
#include "common/Cond.h"
#include "common/Timer.h"
#include "common/ceph_mutex.h"
#include "common/ceph_time.h"
#include "include/common_fwd.h"
#include "include/rados/librados.h"
#include "include/rados/librados.hpp"
#include "mon/MonClient.h"
#include "mgr/MgrClient.h"
#include "msg/Dispatcher.h"

#include "IoCtxImpl.h"

struct AuthAuthorizer;
struct Context;
struct Connection;
class Message;
class MLog;
class Messenger;
class AioCompletionImpl;

class librados::RadosClient : public Dispatcher
{
  std::unique_ptr<CephContext,
		  std::function<void(CephContext*)> > cct_deleter;

public:
/** comment by hy 2020-01-15
 * # 基类中的上下文,是一个指针
 */
  using Dispatcher::cct;
  const ConfigProxy& conf; /* 配置文件 */
private:
  enum {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
  } state;

/** comment by hy 2020-01-15
 * # 初始化需要上下文
 */
  MonClient monclient;
/** comment by hy 2020-01-15
 * # 初始化需要上下文和网络消息
 */
  MgrClient mgrclient;
  Messenger *messenger;
/** comment by hy 2020-01-15
 * # rados 客户端实例由 monitor 分配
 */
  uint64_t instance_id;

  bool _dispatch(Message *m);
  bool ms_dispatch(Message *m) override;

  void ms_handle_connect(Connection *con) override;
  bool ms_handle_reset(Connection *con) override;
  void ms_handle_remote_reset(Connection *con) override;
  bool ms_handle_refused(Connection *con) override;

/** comment by hy 2020-01-15
 * # 初始化为 NULL
 */
  Objecter *objecter;

  ceph::mutex lock = ceph::make_mutex("librados::RadosClient::lock");
  ceph::condition_variable cond;
/** comment by hy 2020-01-15
 * # radosclient 定时任务的全局定时器
 */
  SafeTimer timer;
/** comment by hy 2020-01-15
 * # 初始化为1
 */
  int refcnt;

  version_t log_last_version;
/** comment by hy 2020-01-15
 * # log_cb 初始化时为NULL
 */
  rados_log_callback_t log_cb;
/** comment by hy 2020-01-15
 * # log_cb2 初始化时为NULL
 */
  rados_log_callback2_t log_cb2;
/** comment by hy 2020-01-15
 * # log_cb_arg 初始化时为NULL
 */
  void *log_cb_arg;
  string log_watch;

  bool service_daemon = false;
  string daemon_name, service_name;
  map<string,string> daemon_metadata;

  int wait_for_osdmap();

public:
/** comment by hy 2020-01-15
 * # rados 回调函数调度器
 */
  Finisher finisher;

  explicit RadosClient(CephContext *cct_);
  ~RadosClient() override;
  int ping_monitor(string mon_id, string *result);
/** comment by hy 2020-01-15
 * # 初始化流程
 */
  int connect();
  void shutdown();

  int watch_flush();
  int async_watch_flush(AioCompletionImpl *c);

  uint64_t get_instance_id();

  int get_min_compatible_osd(int8_t* require_osd_release);
  int get_min_compatible_client(int8_t* min_compat_client,
                                int8_t* require_min_compat_client);

  int wait_for_latest_osdmap();

  int create_ioctx(const char *name, IoCtxImpl **io);
  int create_ioctx(int64_t, IoCtxImpl **io);

  int get_fsid(std::string *s);
  int64_t lookup_pool(const char *name);
  bool pool_requires_alignment(int64_t pool_id);
  int pool_requires_alignment2(int64_t pool_id, bool *requires);
  uint64_t pool_required_alignment(int64_t pool_id);
  int pool_required_alignment2(int64_t pool_id, uint64_t *alignment);
  int pool_get_name(uint64_t pool_id, std::string *name,
		    bool wait_latest_map = false);

  int pool_list(std::list<std::pair<int64_t, string> >& ls);
  int get_pool_stats(std::list<string>& ls, map<string,::pool_stat_t> *result,
    bool *per_pool);
  int get_fs_stats(ceph_statfs& result);
  bool get_pool_is_selfmanaged_snaps_mode(const std::string& pool);

  /*
  -1 was set as the default value and monitor will pickup the right crush rule with below order:
    a) osd pool default crush replicated ruleset
    b) the first ruleset in crush ruleset
    c) error out if no value find
  */
  int pool_create(string& name, int16_t crush_rule=-1);
  int pool_create_async(string& name, PoolAsyncCompletionImpl *c,
			int16_t crush_rule=-1);
  int pool_get_base_tier(int64_t pool_id, int64_t* base_tier);
  int pool_delete(const char *name);

  int pool_delete_async(const char *name, PoolAsyncCompletionImpl *c);

  int blacklist_add(const string& client_address, uint32_t expire_seconds);

  int mon_command(const vector<string>& cmd, const bufferlist &inbl,
	          bufferlist *outbl, string *outs);
  void mon_command_async(const vector<string>& cmd, const bufferlist &inbl,
                         bufferlist *outbl, string *outs, Context *on_finish);
  int mon_command(int rank,
		  const vector<string>& cmd, const bufferlist &inbl,
	          bufferlist *outbl, string *outs);
  int mon_command(string name,
		  const vector<string>& cmd, const bufferlist &inbl,
	          bufferlist *outbl, string *outs);
  int mgr_command(const vector<string>& cmd, const bufferlist &inbl,
	          bufferlist *outbl, string *outs);
  int mgr_command(
    const string& name,
    const vector<string>& cmd, const bufferlist &inbl,
    bufferlist *outbl, string *outs);
  int osd_command(int osd, vector<string>& cmd, const bufferlist& inbl,
                  bufferlist *poutbl, string *prs);
  int pg_command(pg_t pgid, vector<string>& cmd, const bufferlist& inbl,
	         bufferlist *poutbl, string *prs);

  void handle_log(MLog *m);
  int monitor_log(const string& level, rados_log_callback_t cb,
		  rados_log_callback2_t cb2, void *arg);

  void get();
  bool put();
  void blacklist_self(bool set);

  std::string get_addrs() const;

  int service_daemon_register(
    const std::string& service,  ///< service name (e.g., 'rgw')
    const std::string& name,     ///< daemon name (e.g., 'gwfoo')
    const std::map<std::string,std::string>& metadata); ///< static metadata about daemon
  int service_daemon_update_status(
    std::map<std::string,std::string>&& status);

  mon_feature_t get_required_monitor_features() const;

  int get_inconsistent_pgs(int64_t pool_id, std::vector<std::string>* pgs);
};

#endif
