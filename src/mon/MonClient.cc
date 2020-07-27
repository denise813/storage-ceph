// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#include <algorithm>
#include <iterator>
#include <random>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/adaptor/filtered.hpp>
#include <boost/range/algorithm/copy.hpp>
#include <boost/range/algorithm_ext/copy_n.hpp>
#include "common/weighted_shuffle.h"

#include "include/scope_guard.h"
#include "include/stringify.h"

#include "messages/MMonGetMap.h"
#include "messages/MMonGetVersion.h"
#include "messages/MMonGetVersionReply.h"
#include "messages/MMonMap.h"
#include "messages/MConfig.h"
#include "messages/MGetConfig.h"
#include "messages/MAuth.h"
#include "messages/MLogAck.h"
#include "messages/MAuthReply.h"
#include "messages/MMonCommand.h"
#include "messages/MMonCommandAck.h"
#include "messages/MCommand.h"
#include "messages/MCommandReply.h"
#include "messages/MPing.h"

#include "messages/MMonSubscribe.h"
#include "messages/MMonSubscribeAck.h"
#include "common/errno.h"
#include "common/hostname.h"
#include "common/LogClient.h"

#include "MonClient.h"
#include "MonMap.h"

#include "auth/Auth.h"
#include "auth/KeyRing.h"
#include "auth/AuthClientHandler.h"
#include "auth/AuthRegistry.h"
#include "auth/RotatingKeyRing.h"

#define dout_subsys ceph_subsys_monc
#undef dout_prefix
#define dout_prefix *_dout << "monclient" << (_hunting() ? "(hunting)":"") << ": "

using std::string;

MonClient::MonClient(CephContext *cct_) :
  Dispatcher(cct_),
  AuthServer(cct_),
  messenger(NULL),
  timer(cct_, monc_lock),
  finisher(cct_),
  initialized(false),
  log_client(NULL),
  more_log_pending(false),
  want_monmap(true),
  had_a_connection(false),
  reopen_interval_multiplier(
    cct_->_conf.get_val<double>("mon_client_hunt_interval_min_multiple")),
  last_mon_command_tid(0),
  version_req_id(0)
{}

MonClient::~MonClient()
{
}

/*****************************************************************************
 * 函 数 名  : MonClient.build_initial_monmap
 * 负 责 人  : hy
 * 创建日期  : 2020年2月18日
 * 函数功能  : 从配置文件中检查是否有初始化的monitor服务
                端地址信息
 * 输入参数  : 无
 * 输出参数  : 无
 * 返 回 值  : int
 * 调用关系  : 
 * 其    它  : 

*****************************************************************************/
int MonClient::build_initial_monmap()
{
  ldout(cct, 10) << __func__ << dendl;
  int r = monmap.build_initial(cct, false, std::cerr);
/** comment by hy 2020-01-15
 * # 打印调试信息,按照指定格式打印信息
 */
  ldout(cct,10) << "monmap:\n";
  monmap.print(*_dout);
  *_dout << dendl;
  return r;
}

int MonClient::get_monmap()
{
  ldout(cct, 10) << __func__ << dendl;
  std::unique_lock l(monc_lock);

  sub.want("monmap", 0, 0);
  if (!_opened())
    _reopen_session();
  map_cond.wait(l, [this] { return !want_monmap; });
  ldout(cct, 10) << __func__ << " done" << dendl;
  return 0;
}

int MonClient::get_monmap_and_config()
{
/** comment by hy 2020-01-15
 * # 调试信息
 */
  ldout(cct, 10) << __func__ << dendl;
  ceph_assert(!messenger);

  int tries = 10;

  cct->init_crypto();
/** comment by hy 2020-01-15
 * # librados的环境,初始化阶段就不认证？
 */
  auto shutdown_crypto = make_scope_guard([this] {
    cct->shutdown_crypto();
  });
/** comment by hy 2020-01-15
 * # 从配置文件中构建monmap信息
 */
  int r = build_initial_monmap();
  if (r < 0) {
    lderr(cct) << __func__ << " cannot identify monitors to contact" << dendl;
    return r;
  }

/** comment by hy 2020-01-15
 * # 创建一个临时通路
 */
  messenger = Messenger::create_client_messenger(
    cct, "temp_mon_client");
  ceph_assert(messenger);
/** comment by hy 2020-01-15
 * # 往dispatcher的头部加入分发者,分发则用来调用消息处理函数
 */
  messenger->add_dispatcher_head(this);
/** comment by hy 2020-01-15
 * # 启动通路
 */
  messenger->start();
/** comment by hy 2020-01-15
 * # 已经取得mmonmap信息,就关闭临时的通路
 */
  auto shutdown_msgr = make_scope_guard([this] {
    messenger->shutdown();
    messenger->wait();
    delete messenger;
    messenger = nullptr;
    if (!monmap.fsid.is_zero()) {
      cct->_conf.set_val("fsid", stringify(monmap.fsid));
    }
  });

  while (tries-- > 0) {
/** comment by hy 2020-01-15
 * # 执行初始化,这里调用了 ready 开始监控网络
 */
    r = init();
    if (r < 0) {
      return r;
    }
/** comment by hy 2020-01-16
 * # 开始进行证书认证,认证失败就重连
 */
    r = authenticate(cct->_conf->client_mount_timeout);
    if (r == -ETIMEDOUT) {
      shutdown();
      continue;
    }
/** comment by hy 2020-01-16
 * # 失败了也就不重试了
         这里是不是没有考虑网络不通,没有调用 shutdown
 */
    if (r < 0) {
      break;
    }
    {
      std::unique_lock l(monc_lock);
/** comment by hy 2020-01-17
 * # 在 mimic 版本特性
 */
      if (monmap.get_epoch() &&
	  !monmap.persistent_features.contains_all(
	    ceph::features::mon::FEATURE_MIMIC)) {
	ldout(cct,10) << __func__ << " pre-mimic monitor, no config to fetch"
		      << dendl;
	r = 0;
	break;
      }
/** comment by hy 2020-01-17
 * # 等待monmap 推送过来,有超时时间
 */
      while ((!got_config || monmap.get_epoch() == 0) && r == 0) {
	ldout(cct,20) << __func__ << " waiting for monmap|config" << dendl;
	map_cond.wait_for(l, ceph::make_timespan(
          cct->_conf->mon_client_hunt_interval));
      }
/** comment by hy 2020-01-17
 * # 超时时间到达后检查config得到更新
 */
      if (got_config) {
	ldout(cct,10) << __func__ << " success" << dendl;
	r = 0;
	break;
      }
    }
/** comment by hy 2020-01-17
 * # 还没更新,就关闭网络进行重试
 */
    lderr(cct) << __func__ << " failed to get config" << dendl;
    shutdown();
    continue;
  }

/** comment by hy 2020-01-17
 * # 成功后关闭临时通路
 */
  shutdown();
  return r;
}


/**
 * Ping the monitor with id @p mon_id and set the resulting reply in
 * the provided @p result_reply, if this last parameter is not NULL.
 *
 * So that we don't rely on the MonClient's default messenger, set up
 * during connect(), we create our own messenger to comunicate with the
 * specified monitor.  This is advantageous in the following ways:
 *
 * - Isolate the ping procedure from the rest of the MonClient's operations,
 *   allowing us to not acquire or manage the big monc_lock, thus not
 *   having to block waiting for some other operation to finish before we
 *   can proceed.
 *   * for instance, we can ping mon.FOO even if we are currently hunting
 *     or blocked waiting for auth to complete with mon.BAR.
 *
 * - Ping a monitor prior to establishing a connection (using connect())
 *   and properly establish the MonClient's messenger.  This frees us
 *   from dealing with the complex foo that happens in connect().
 *
 * We also don't rely on MonClient as a dispatcher for this messenger,
 * unlike what happens with the MonClient's default messenger.  This allows
 * us to sandbox the whole ping, having it much as a separate entity in
 * the MonClient class, considerably simplifying the handling and dispatching
 * of messages without needing to consider monc_lock.
 *
 * Current drawback is that we will establish a messenger for each ping
 * we want to issue, instead of keeping a single messenger instance that
 * would be used for all pings.
 */
int MonClient::ping_monitor(const string &mon_id, string *result_reply)
{
  ldout(cct, 10) << __func__ << dendl;

  string new_mon_id;
  if (monmap.contains("noname-"+mon_id)) {
    new_mon_id = "noname-"+mon_id;
  } else {
    new_mon_id = mon_id;
  }

  if (new_mon_id.empty()) {
    ldout(cct, 10) << __func__ << " specified mon id is empty!" << dendl;
    return -EINVAL;
  } else if (!monmap.contains(new_mon_id)) {
    ldout(cct, 10) << __func__ << " no such monitor 'mon." << new_mon_id << "'"
                   << dendl;
    return -ENOENT;
  }

  // N.B. monc isn't initialized

  auth_registry.refresh_config();

  KeyRing keyring;
  keyring.from_ceph_context(cct);
  RotatingKeyRing rkeyring(cct, cct->get_module_type(), &keyring);

  MonClientPinger *pinger = new MonClientPinger(cct,
						&rkeyring,
						result_reply);

  Messenger *smsgr = Messenger::create_client_messenger(cct, "temp_ping_client");
  smsgr->add_dispatcher_head(pinger);
  smsgr->set_auth_client(pinger);
  smsgr->start();

  ConnectionRef con = smsgr->connect_to_mon(monmap.get_addrs(new_mon_id));
  ldout(cct, 10) << __func__ << " ping mon." << new_mon_id
                 << " " << con->get_peer_addr() << dendl;

  pinger->mc.reset(new MonConnection(cct, con, 0, &auth_registry));
  pinger->mc->start(monmap.get_epoch(), entity_name);
  con->send_message(new MPing);

  int ret = pinger->wait_for_reply(cct->_conf->mon_client_ping_timeout);
  if (ret == 0) {
    ldout(cct,10) << __func__ << " got ping reply" << dendl;
  } else {
    ret = -ret;
  }

  con->mark_down();
  pinger->mc.reset();
  smsgr->shutdown();
  smsgr->wait();
  delete smsgr;
  delete pinger;
  return ret;
}

bool MonClient::ms_dispatch(Message *m)
{
  // we only care about these message types
  switch (m->get_type()) {
  case CEPH_MSG_MON_MAP:
  case CEPH_MSG_AUTH_REPLY:
  case CEPH_MSG_MON_SUBSCRIBE_ACK:
  case CEPH_MSG_MON_GET_VERSION_REPLY:
  case MSG_MON_COMMAND_ACK:
  case MSG_COMMAND_REPLY:
  case MSG_LOGACK:
  case MSG_CONFIG:
    break;
  case CEPH_MSG_PING:
    m->put();
    return true;
  default:
    return false;
  }

  std::lock_guard lock(monc_lock);

  if (!m->get_connection()->is_anon() &&
      m->get_source().type() == CEPH_ENTITY_TYPE_MON) {
    if (_hunting()) {
      auto p = _find_pending_con(m->get_connection());
      if (p == pending_cons.end()) {
	// ignore any messages outside hunting sessions
	ldout(cct, 10) << "discarding stray monitor message " << *m << dendl;
	m->put();
	return true;
      }
    } else if (!active_con || active_con->get_con() != m->get_connection()) {
      // ignore any messages outside our session(s)
      ldout(cct, 10) << "discarding stray monitor message " << *m << dendl;
      m->put();
      return true;
    }
  }

  switch (m->get_type()) {
  case CEPH_MSG_MON_MAP:
    handle_monmap(static_cast<MMonMap*>(m));
    if (passthrough_monmap) {
      return false;
    } else {
      m->put();
    }
    break;
  case CEPH_MSG_AUTH_REPLY:
    handle_auth(static_cast<MAuthReply*>(m));
    break;
  case CEPH_MSG_MON_SUBSCRIBE_ACK:
    handle_subscribe_ack(static_cast<MMonSubscribeAck*>(m));
    break;
  case CEPH_MSG_MON_GET_VERSION_REPLY:
    handle_get_version_reply(static_cast<MMonGetVersionReply*>(m));
    break;
  case MSG_MON_COMMAND_ACK:
    handle_mon_command_ack(static_cast<MMonCommandAck*>(m));
    break;
  case MSG_COMMAND_REPLY:
    if (m->get_connection()->is_anon() &&
        m->get_source().type() == CEPH_ENTITY_TYPE_MON) {
      // this connection is from 'tell'... ignore everything except our command
      // reply.  (we'll get misc other message because we authenticated, but we
      // don't need them.)
      handle_command_reply(static_cast<MCommandReply*>(m));
      return true;
    }
    // leave the message for another dispatch handler (e.g., Objecter)
    return false;
  case MSG_LOGACK:
    if (log_client) {
      log_client->handle_log_ack(static_cast<MLogAck*>(m));
      m->put();
      if (more_log_pending) {
	send_log();
      }
    } else {
      m->put();
    }
    break;
  case MSG_CONFIG:
    handle_config(static_cast<MConfig*>(m));
    break;
  }
  return true;
}

void MonClient::send_log(bool flush)
{
/** comment by hy 2020-01-16
 * # 默认参数 flush = false
      只要不是退出状态 log_client 就不为空
 */
  if (log_client) {
/** comment by hy 2020-01-16
 * # 封装推送log信息的消息
 */
    auto lm = log_client->get_mon_log_message(flush);
    if (lm)
/** comment by hy 2020-01-16
 * # 发送成功
 */
      _send_mon_message(std::move(lm));
/** comment by hy 2020-01-16
 * # 设置是否还有log信息还没被推送标识
 */
    more_log_pending = log_client->are_pending();
  }
}

void MonClient::flush_log()
{
  std::lock_guard l(monc_lock);
  send_log();
}

/* Unlike all the other message-handling functions, we don't put away a reference
* because we want to support MMonMap passthrough to other Dispatchers. */
void MonClient::handle_monmap(MMonMap *m)
{
  ldout(cct, 10) << __func__ << " " << *m << dendl;
  auto con_addrs = m->get_source_addrs();
  string old_name = monmap.get_name(con_addrs);
  const auto old_epoch = monmap.get_epoch();

  auto p = m->monmapbl.cbegin();
  decode(monmap, p);

  ldout(cct, 10) << " got monmap " << monmap.epoch
		 << " from mon." << old_name
		 << " (according to old e" << monmap.get_epoch() << ")"
 		 << dendl;
  ldout(cct, 10) << "dump:\n";
  monmap.print(*_dout);
  *_dout << dendl;

  if (old_epoch != monmap.get_epoch()) {
    tried.clear();
  }
  if (old_name.size() == 0) {
    ldout(cct,10) << " can't identify which mon we were connected to" << dendl;
    _reopen_session();
  } else {
    auto new_name = monmap.get_name(con_addrs);
    if (new_name.empty()) {
      ldout(cct, 10) << "mon." << old_name << " at " << con_addrs
		     << " went away" << dendl;
      // can't find the mon we were talking to (above)
      _reopen_session();
    } else if (messenger->should_use_msgr2() &&
	       monmap.get_addrs(new_name).has_msgr2() &&
	       !con_addrs.has_msgr2()) {
      ldout(cct,1) << " mon." << new_name << " has (v2) addrs "
		   << monmap.get_addrs(new_name) << " but i'm connected to "
		   << con_addrs << ", reconnecting" << dendl;
      _reopen_session();
    }
  }

  sub.got("monmap", monmap.get_epoch());
  map_cond.notify_all();
  want_monmap = false;

  if (authenticate_err == 1) {
    _finish_auth(0);
  }
}

void MonClient::handle_config(MConfig *m)
{
  ldout(cct,10) << __func__ << " " << *m << dendl;
  finisher.queue(new LambdaContext([this, m](int r) {
	cct->_conf.set_mon_vals(cct, m->config, config_cb);
	if (config_notify_cb) {
	  config_notify_cb();
	}
	m->put();
      }));
  got_config = true;
  map_cond.notify_all();
}

// ----------------------

int MonClient::init()
{
  ldout(cct, 10) << __func__ << dendl;

  entity_name = cct->_conf->name;

/** comment by hy 2020-01-16
 * # 根据配置文件更新被认证的权限
 */
  auth_registry.refresh_config();

/** comment by hy 2020-01-16
 * # 获取证书
 */
  std::lock_guard l(monc_lock);
  keyring.reset(new KeyRing);
/** comment by hy 2020-01-16
 * # 使用的是默认的ceph认证
 */
  if (auth_registry.is_supported_method(messenger->get_mytype(),
					CEPH_AUTH_CEPHX)) {
    // this should succeed, because auth_registry just checked!
/** comment by hy 2020-01-16
 * # KeyRing()->from_ceph_context() 是从配置文件制定的位置读取秘钥文件
 */
    int r = keyring->from_ceph_context(cct);
    if (r != 0) {
      // but be somewhat graceful in case there was a race condition
      lderr(cct) << "keyring not found" << dendl;
      return r;
    }
  }
/** comment by hy 2020-01-16
 * # 如果没有获取到改模块对应认证,即没有被认证
 */
  if (!auth_registry.any_supported_methods(messenger->get_mytype())) {
    return -ENOENT;
  }

/** comment by hy 2020-01-16
 * # 将模块秘钥设置到秘钥认证指针数组中
 */
  rotating_secrets.reset(
    new RotatingKeyRing(cct, cct->get_module_type(), keyring.get()));

  initialized = true;

/** comment by hy 2020-01-16
 * # 使能认证相关接口
 */
  messenger->set_auth_client(this);
/** comment by hy 2020-04-07
 * # 将分发放入接收处理中
 */
  messenger->add_dispatcher_head(this);

/** comment by hy 2020-01-16
 * # 准备定时任务的定时器
 */
  timer.init();
/** comment by hy 2020-01-16
 * # 准备异步回调
 */
  finisher.start();
/** comment by hy 2020-01-16
 * # 启动定时任务
 */
  schedule_tick();

  return 0;
}

void MonClient::shutdown()
{
/** comment by hy 2020-01-17
 * # 调试信息
 */
  ldout(cct, 10) << __func__ << dendl;
  monc_lock.lock();
/** comment by hy 2020-01-17
 * # 设置停止中标志位
 */
  stopping = true;
/** comment by hy 2020-01-17
 * # 取消等候版本信息完成
 */
  while (!version_requests.empty()) {
    version_requests.begin()->second->context->complete(-ECANCELED);
    ldout(cct, 20) << __func__ << " canceling and discarding version request "
		   << version_requests.begin()->second << dendl;
    delete version_requests.begin()->second;
    version_requests.erase(version_requests.begin());
  }
/** comment by hy 2020-01-17
 * # 取消等候命令行
 */
  while (!mon_commands.empty()) {
    auto tid = mon_commands.begin()->first;
    _cancel_mon_command(tid);
  }
  ldout(cct, 20) << __func__ << " discarding " << waiting_for_session.size()
/** comment by hy 2020-01-17
 * # 取消等候 session
 */
		 << " pending message(s)" << dendl;
  waiting_for_session.clear();

  active_con.reset();
  pending_cons.clear();
  auth.reset();

  monc_lock.unlock();

  if (initialized) {
    finisher.wait_for_empty();
    finisher.stop();
    initialized = false;
  }
/** comment by hy 2020-01-17
 * # 设置停止结束
 */
  monc_lock.lock();
  timer.shutdown();
  stopping = false;
  monc_lock.unlock();
}

int MonClient::authenticate(double timeout)
{
  std::unique_lock lock{monc_lock};

/** comment by hy 2020-01-16
 * # 如果有已经稳定并认证确保具有权限的连接,那么久不用继续认证
         返回成功
         初始化过程是不会存在的
 */
  if (active_con) {
    ldout(cct, 5) << "already authenticated" << dendl;
    return 0;
  }
/** comment by hy 2020-01-16
 * # 向mon定于比当前还要新一个版本的mon拓扑结构
         如果初始化极端没有mon的版本信息,就从0号版本开始吧
 */
  sub.want("monmap", monmap.get_epoch() ? monmap.get_epoch() + 1 : 0, 0);
/** comment by hy 2020-01-16
 * # 要配置文件
 */
  sub.want("config", 0, 0);
/** comment by hy 2020-01-16
 * # 检查是不是已经建立连接,如果没有就新建立连接 session
         在建立会话中
 */
  if (!_opened())
    _reopen_session();

  auto until = ceph::real_clock::now();
  until += ceph::make_timespan(timeout);
  if (timeout > 0.0)
    ldout(cct, 10) << "authenticate will time out at " << until << dendl;
  authenticate_err = 1;  // == in progress
  while (!active_con && authenticate_err >= 0) {
/** comment by hy 2020-01-16
 * # 如果设置了超时时间,则超过指定超时时间则认为认证失败
 */
    if (timeout > 0.0) {
      auto r = auth_cond.wait_until(lock, until);
      if (r == cv_status::timeout && !active_con) {
	ldout(cct, 0) << "authenticate timed out after " << timeout << dendl;
	authenticate_err = -ETIMEDOUT;
      }
    } else {
/** comment by hy 2020-01-16
 * # 等待接受认证应答消息,唤醒继续
 */
      auth_cond.wait(lock);
    }
  }

/** comment by hy 2020-01-16
 * # 以下是异步等待唤醒后执行,该状态一定已经认证完成
         设置以及认证标识
 */
  if (active_con) {
    ldout(cct, 5) << __func__ << " success, global_id "
		  << active_con->get_global_id() << dendl;
    // active_con should not have been set if there was an error
    ceph_assert(authenticate_err >= 0);
    authenticated = true;
  }

/** comment by hy 2020-01-16
 * # 如果没认证成功, 或者认证没关闭,但是没key
 */
  if (authenticate_err < 0 && auth_registry.no_keyring_disabled_cephx()) {
    lderr(cct) << __func__ << " NOTE: no keyring found; disabled cephx authentication" << dendl;
  }

  return authenticate_err;
}

void MonClient::handle_auth(MAuthReply *m)
{
  ceph_assert(ceph_mutex_is_locked(monc_lock));

  if (m->get_connection()->is_anon()) {
    // anon connection, used for mon tell commands
    for (auto& p : mon_commands) {
      if (p.second->target_con == m->get_connection()) {
	auto& mc = p.second->target_session;
	int ret = mc->handle_auth(m, entity_name,
				  CEPH_ENTITY_TYPE_MON,
				  rotating_secrets.get());
	(void)ret; // we don't care
	break;
      }
    }
    m->put();
    return;
  }

  if (!_hunting()) {
    std::swap(active_con->get_auth(), auth);
    int ret = active_con->authenticate(m);
    m->put();
    std::swap(auth, active_con->get_auth());
    if (global_id != active_con->get_global_id()) {
      lderr(cct) << __func__ << " peer assigned me a different global_id: "
		 << active_con->get_global_id() << dendl;
    }
    if (ret != -EAGAIN) {
      _finish_auth(ret);
    }
    return;
  }

  // hunting
  auto found = _find_pending_con(m->get_connection());
  ceph_assert(found != pending_cons.end());
  int auth_err = found->second.handle_auth(m, entity_name, want_keys,
					   rotating_secrets.get());
  m->put();
  if (auth_err == -EAGAIN) {
    return;
  }
  if (auth_err) {
/** comment by hy 2019-12-30
 * # 认证失败
 */
    pending_cons.erase(found);
    if (!pending_cons.empty()) {
      // keep trying with pending connections
      return;
    }
    // the last try just failed, give up.
  } else {
    auto& mc = found->second;
    ceph_assert(mc.have_session());
/** comment by hy 2019-12-30
 * # 更新连接
 */
    active_con.reset(new MonConnection(std::move(mc)));
    pending_cons.clear();
  }

  _finish_hunting(auth_err);
  _finish_auth(auth_err);
}

void MonClient::_finish_auth(int auth_err)
{
  ldout(cct,10) << __func__ << " " << auth_err << dendl;
  authenticate_err = auth_err;
  // _resend_mon_commands() could _reopen_session() if the connected mon is not
  // the one the MonCommand is targeting.
  if (!auth_err && active_con) {
    ceph_assert(auth);
    _check_auth_tickets();
  }
  auth_cond.notify_all();

  if (!auth_err) {
    Context *cb = nullptr;
    if (session_established_context) {
      cb = session_established_context.release();
    }
    if (cb) {
      monc_lock.unlock();
      cb->complete(0);
      monc_lock.lock();
    }
  }
}

// ---------

void MonClient::send_mon_message(MessageRef m)
{
  std::lock_guard l{monc_lock};
  _send_mon_message(std::move(m));
}

void MonClient::_send_mon_message(MessageRef m)
{
  ceph_assert(ceph_mutex_is_locked(monc_lock));
/** comment by hy 2020-01-16
 * # 有链接就用连接发送
 */
  if (active_con) {
    auto cur_con = active_con->get_con();
    ldout(cct, 10) << "_send_mon_message to mon."
		   << monmap.get_name(cur_con->get_peer_addr())
		   << " at " << cur_con->get_peer_addr() << dendl;
    cur_con->send_message2(std::move(m));
  } else {
/** comment by hy 2019-12-30
 * # 与 reopen_session 相关
 */
    waiting_for_session.push_back(std::move(m));
  }
}

void MonClient::_reopen_session(int rank)
{
/** comment by hy 2020-01-16
 * # 调试信息
 */
  ceph_assert(ceph_mutex_is_locked(monc_lock));
  ldout(cct, 10) << __func__ << " rank " << rank << dendl;

/** comment by hy 2020-01-16
 * # 连接重置
 */
  active_con.reset();
  pending_cons.clear();

/** comment by hy 2020-01-16
 * # 设置探测mon状态的间隔
 */
  _start_hunting();

/** comment by hy 2020-01-16
 * # 改函数具有默认参数 为-1      
 */
  if (rank >= 0) {
/** comment by hy 2020-01-16
 * # 与指定mon建立连接
 */
    _add_conn(rank, global_id);
  } else {
/** comment by hy 2020-01-16
 * # 
 */
    _add_conns(global_id);
  }

  // throw out old queued messages
/** comment by hy 2020-01-16
 * # 由于发送消息时，没有一个有效的连接,
         所有因为上述原因而不合适进行发送的连接将放在 
     waiting_for_session 容器中
     由于这是新打开一个会话,所有上一个会话的消息都将丢弃
     实际结果不过是定时建立会话,建立一次就丢弃请求
     现阶段是丢弃,这样与以前的版本逻辑相同,只管发送不管埋一个逻辑
     是不是这里想憋什么招式
 */
  waiting_for_session.clear();

  // throw out version check requests
/** comment by hy 2020-01-16
 * # 只有后续向 mon发送想要版本请求的消息,
         这里的版本信息说的是 osdmap, fsmap等集群拓扑 的版本信息
         才这个容器 version_requests 添加实例
         该实例是异步等待版本应答消息后执行器回调
 */
  while (!version_requests.empty()) {
/** comment by hy 2020-01-16
 * # 放入回调管理器中
         为什么不先摘除后放入队列?有问题？
 */
    finisher.queue(version_requests.begin()->second->context, -EAGAIN);
/** comment by hy 2020-01-16
 * # 清理资源
 */
    delete version_requests.begin()->second;
    version_requests.erase(version_requests.begin());
  }

/** comment by hy 2020-01-16
 * # 连接还为得到认证
 */
  for (auto& c : pending_cons) {
/** comment by hy 2020-01-16
 * # MonConnection::start() ,获取monmap 信息,并且试着再次发送认证消息
     发送 keepalive and auth 消息
 */
    c.second.start(monmap.get_epoch(), entity_name);
  }

/** comment by hy 2020-01-16
 * # 保持原有的定于信息,并添加新的订阅信息
         发送订阅消息,并进行订阅
 */
  if (sub.reload()) {
    _renew_subs();
  }
}

MonConnection& MonClient::_add_conn(unsigned rank, uint64_t global_id)
{
/** comment by hy 2020-01-16
 * # 获取与指定mon地址,因为与mon进行连接,所以相对于该客户端mon就是peer
 */
  auto peer = monmap.get_addrs(rank);
/** comment by hy 2020-01-16
 * # 建立连接
 */
  auto conn = messenger->connect_to_mon(peer);
/** comment by hy 2020-01-16
 * # 将连接组装成mon连接对象
 */
  MonConnection mc(cct, conn, global_id, &auth_registry);
/** comment by hy 2020-01-16
 * # 将<地址, mc>放入等待认证确认队列中
     建立连接后将连接放入检查表中
 */
  auto inserted = pending_cons.insert(std::make_pair(peer, std::move(mc)));
  ldout(cct, 10) << "picked mon." << monmap.get_name(rank)
                 << " con " << conn
                 << " addr " << peer
                 << dendl;
/** comment by hy 2020-01-16
 * # 返回对应的连接对象
 */
  return inserted.first->second;
}

void MonClient::_add_conns(uint64_t global_id)
{
  // collect the next batch of candidates who are listed right next to the ones
  // already tried
/** comment by hy 2020-01-16
 * # 注册函数 get_next_batch
         获取rank连接信息,并转化好对应格式,最终插入到优先级容器中
         如果失败将返回空字典
 */
  auto get_next_batch = [this]() -> std::vector<unsigned> {
    std::multimap<uint16_t, unsigned> ranks_by_priority;
    boost::copy(
      monmap.mon_info | boost::adaptors::filtered(
/** comment by hy 2020-01-16
 * # 过滤规则使用匿名函数,
         该函数获取mon对应的rank信息
         并返回成功获得信息的bool标志位
     如果已经与某mon建立连接,对应的tried将不为空
 */
        [this](auto& info) {
          auto rank = monmap.get_rank(info.first);
          return tried.count(rank) == 0;
        }) | boost::adaptors::transformed(
          [this](auto& info) {
/** comment by hy 2020-01-16
 * # 转化成对应的键值对形式
 */
            auto rank = monmap.get_rank(info.first);
            return std::make_pair(info.second.priority, rank);
          }), std::inserter(ranks_by_priority, end(ranks_by_priority)));
    if (ranks_by_priority.empty()) {
      return {};
    }
    // only choose the monitors with lowest priority
    auto cands = boost::make_iterator_range(
      ranks_by_priority.equal_range(ranks_by_priority.begin()->first));
    std::vector<unsigned> ranks;
    boost::range::copy(cands | boost::adaptors::map_values,
		       std::back_inserter(ranks));
    return ranks;
  };
/** comment by hy 2020-01-16
 * # 调用注册的匿名函数,获得不成功,再获得一次
 */
  auto ranks = get_next_batch();
  if (ranks.empty()) {
    tried.clear();  // start over
    ranks = get_next_batch();
  }
  ceph_assert(!ranks.empty());
  if (ranks.size() > 1) {
/** comment by hy 2020-01-16
 * # 获取mon对应的权重
 */
    std::vector<uint16_t> weights;
    for (auto i : ranks) {
      auto rank_name = monmap.get_name(i);
      weights.push_back(monmap.get_weight(rank_name));
    }
    std::random_device rd;
/** comment by hy 2020-01-16
 * # 如果随机排列ranks
 */
    if (std::accumulate(begin(weights), end(weights), 0u) == 0) {
/** comment by hy 2020-01-16
 * # 如果mon列表所有权重和等于0
         按照rank为key进行随机排列
 */
      std::shuffle(begin(ranks), end(ranks), std::mt19937{rd()});
    } else {
/** comment by hy 2020-01-16
 * # 如果有权重 按照权重作为key进行rank随机排列
 */
      weighted_shuffle(begin(ranks), end(ranks), begin(weights), end(weights),
		       std::mt19937{rd()});
    }
  }
  ldout(cct, 10) << __func__ << " ranks=" << ranks << dendl;
/** comment by hy 2020-01-16
 * # 当 hunting 住 试着向多个 mon 进行试探连接
 */
  unsigned n = cct->_conf->mon_client_hunt_parallel;
  if (n == 0 || n > ranks.size()) {
    n = ranks.size();
  }
  for (unsigned i = 0; i < n; i++) {
/** comment by hy 2020-01-16
 * # 向每个rank建立连接,尝试过的rank保存在临时容器中
         只有收到monmap信息才清除这种尝试
 */
    _add_conn(ranks[i], global_id);
    tried.insert(ranks[i]);
  }
}

bool MonClient::ms_handle_reset(Connection *con)
{
  std::lock_guard lock(monc_lock);

  if (con->get_peer_type() != CEPH_ENTITY_TYPE_MON)
    return false;

  if (con->is_anon()) {
    auto p = mon_commands.begin();
    while (p != mon_commands.end()) {
      auto cmd = p->second;
      ++p;
      if (cmd->target_con == con) {
	_send_command(cmd); // may retry or fail
	break;
      }
    }
    return true;
  }

  if (_hunting()) {
    if (pending_cons.count(con->get_peer_addrs())) {
      ldout(cct, 10) << __func__ << " hunted mon " << con->get_peer_addrs()
		     << dendl;
    } else {
      ldout(cct, 10) << __func__ << " stray mon " << con->get_peer_addrs()
		     << dendl;
    }
    return true;
  } else {
    if (active_con && con == active_con->get_con()) {
      ldout(cct, 10) << __func__ << " current mon " << con->get_peer_addrs()
		     << dendl;
      _reopen_session();
      return false;
    } else {
      ldout(cct, 10) << "ms_handle_reset stray mon " << con->get_peer_addrs()
		     << dendl;
      return true;
    }
  }
}

bool MonClient::_opened() const
{
  ceph_assert(ceph_mutex_is_locked(monc_lock));
  return active_con || _hunting();
}

bool MonClient::_hunting() const
{
  return !pending_cons.empty();
}

void MonClient::_start_hunting()
{
  ceph_assert(!_hunting());
  // adjust timeouts if necessary
  if (!had_a_connection)
    return;
/** comment by hy 2020-01-16
 * # 没有连接不应该是缩小,这个不应该是放在 finish_hunting中?
         这是一个初始执行的过程,
         如果是我,我会使用一个初始值这个值等于mon
         这个理解有待检验
 */
  reopen_interval_multiplier *= cct->_conf->mon_client_hunt_interval_backoff;
  if (reopen_interval_multiplier >
      cct->_conf->mon_client_hunt_interval_max_multiple) {
    reopen_interval_multiplier =
      cct->_conf->mon_client_hunt_interval_max_multiple;
  }
}

void MonClient::_finish_hunting(int auth_err)
{
  ldout(cct,10) << __func__ << " " << auth_err << dendl;
  ceph_assert(ceph_mutex_is_locked(monc_lock));
  // the pending conns have been cleaned.
  ceph_assert(!_hunting());
  if (active_con) {
    auto con = active_con->get_con();
    ldout(cct, 1) << "found mon."
		  << monmap.get_name(con->get_peer_addr())
		  << dendl;
  } else {
    ldout(cct, 1) << "no mon sessions established" << dendl;
  }

  had_a_connection = true;
  _un_backoff();

  if (!auth_err) {
    last_rotating_renew_sent = utime_t();
    while (!waiting_for_session.empty()) {
      _send_mon_message(std::move(waiting_for_session.front()));
      waiting_for_session.pop_front();
    }
    _resend_mon_commands();
    send_log(true);
    if (active_con) {
      std::swap(auth, active_con->get_auth());
      if (global_id && global_id != active_con->get_global_id()) {
	lderr(cct) << __func__ << " global_id changed from " << global_id
		   << " to " << active_con->get_global_id() << dendl;
      }
      global_id = active_con->get_global_id();
    }
  }
}

void MonClient::tick()
{
  ldout(cct, 10) << __func__ << dendl;

  utime_t now = ceph_clock_now();
/** comment by hy 2020-01-16
 * # 形成回环,达到不间断地调用
 */
  auto reschedule_tick = make_scope_guard([this] {
      schedule_tick();
    });

/** comment by hy 2020-01-16
 * # 如果使用令牌认证,则按照令牌认证方式进行认证
     定时 check
 */
  _check_auth_tickets();
  _check_tell_commands();
/** comment by hy 2020-01-16
 * # 如果还是没有通过认证,重新建立连接吧
 */
  if (_hunting()) {
    ldout(cct, 1) << "continuing hunt" << dendl;
    return _reopen_session();
  } else if (active_con) {
    // just renew as needed
/** comment by hy 2020-01-16
 * # 如果有新订阅信息,更新订阅
 */
    auto cur_con = active_con->get_con();
    if (!cur_con->has_feature(CEPH_FEATURE_MON_STATEFUL_SUB)) {
      const bool maybe_renew = sub.need_renew();
      ldout(cct, 10) << "renew subs? -- " << (maybe_renew ? "yes" : "no")
		     << dendl;
      if (maybe_renew) {
/** comment by hy 2020-01-16
 * # 进行推送新信息
 */
	_renew_subs();
      }
    }

    if (now > last_keepalive + cct->_conf->mon_client_ping_interval) {
      cur_con->send_keepalive();
      last_keepalive = now;
/** comment by hy 2020-01-16
 * # 使用keepalive来检查 ping mon 
 */
      if (cct->_conf->mon_client_ping_timeout > 0 &&
	  cur_con->has_feature(CEPH_FEATURE_MSGR_KEEPALIVE2)) {
	utime_t lk = cur_con->get_last_keepalive_ack();
	utime_t interval = now - lk;
	if (interval > cct->_conf->mon_client_ping_timeout) {
	  ldout(cct, 1) << "no keepalive since " << lk << " (" << interval
			<< " seconds), reconnecting" << dendl;
	  return _reopen_session();
	}
      }
/** comment by hy 2020-01-16
 * # 更新 探测间隔 正常通路就不断加大间隔,异常时就不断缩小间隔
         现在处于异常中
 */
      _un_backoff();
    }

    if (now > last_send_log + cct->_conf->mon_client_log_interval) {
/** comment by hy 2020-01-16
 * # 推送 log信息
 */
      send_log();
      last_send_log = now;
    }
  }
}

void MonClient::_un_backoff()
{
  // un-backoff our reconnect interval
  reopen_interval_multiplier = std::max(
    cct->_conf.get_val<double>("mon_client_hunt_interval_min_multiple"),
    reopen_interval_multiplier /
    cct->_conf.get_val<double>("mon_client_hunt_interval_backoff"));
  ldout(cct, 20) << __func__ << " reopen_interval_multipler now "
		 << reopen_interval_multiplier << dendl;
}

void MonClient::schedule_tick()
{
/** comment by hy 2020-01-16
 * # 执行定时任务
 */
  auto do_tick = make_lambda_context([this](int) { tick(); });
/** comment by hy 2020-01-16
 * # 处于hunting状态进行定时连接 mon
     否则ping一下mon,
     tick() 包罗了上面两个流程
 */
  if (_hunting()) {
    const auto hunt_interval = (cct->_conf->mon_client_hunt_interval *
				reopen_interval_multiplier);
    timer.add_event_after(hunt_interval, do_tick);
  } else {
    timer.add_event_after(std::min(cct->_conf->mon_client_ping_interval,
				   cct->_conf->mon_client_log_interval),
			  do_tick);
  }
}

// ---------

void MonClient::_renew_subs()
{
  ceph_assert(ceph_mutex_is_locked(monc_lock));
/** comment by hy 2020-01-16
 * # 没有新东西了
 */
  if (!sub.have_new()) {
    ldout(cct, 10) << __func__ << " - empty" << dendl;
    return;
  }

  ldout(cct, 10) << __func__ << dendl;
/** comment by hy 2020-01-16
 * # 更新订阅前,看看连接状态
 */
  if (!_opened())
    _reopen_session();
  else {
/** comment by hy 2020-01-16
 * # 发送订阅消息
 */
    auto m = ceph::make_message<MMonSubscribe>();
    m->what = sub.get_subs();
    m->hostname = ceph_get_short_hostname();
    _send_mon_message(std::move(m));
/** comment by hy 2020-01-16
 * # 清除新订阅,并把新订阅的东西放在 sub_sent 容器中
 */
    sub.renewed();
  }
}

void MonClient::handle_subscribe_ack(MMonSubscribeAck *m)
{
  sub.acked(m->interval);
  m->put();
}

int MonClient::_check_auth_tickets()
{
  ceph_assert(ceph_mutex_is_locked(monc_lock));
  if (active_con && auth) {
/** comment by hy 2020-01-16
 * # 如果使用令牌认证,则按照令牌认证的方式包装消息并发送
 */
    if (auth->need_tickets()) {
      ldout(cct, 10) << __func__ << " getting new tickets!" << dendl;
      auto m = ceph::make_message<MAuth>();
      m->protocol = auth->get_protocol();
      auth->prepare_build_request();
      auth->build_request(m->auth_payload);
      _send_mon_message(m);
    }

/** comment by hy 2020-01-16
 * # 循环更新证书,到了时间就使用新证书进行认证
 */
    _check_auth_rotating();
  }
  return 0;
}

/*****************************************************************************
 * 函 数 名  : MonClient._check_auth_rotating
 * 负 责 人  : hy
 * 创建日期  : 2020年1月16日
 * 函数功能  : 使用证书环进行认证
 * 输入参数  : 无
 * 输出参数  : 无
 * 返 回 值  : int  返回值在该函数中不进行参考
 * 调用关系  : 
 * 其    它  : 

*****************************************************************************/
int MonClient::_check_auth_rotating()
{
  ceph_assert(ceph_mutex_is_locked(monc_lock));
/** comment by hy 2020-01-16
 * # 如果没有证书,或者不使用证书环返回成功
 */
  if (!rotating_secrets ||
      !auth_principal_needs_rotating_keys(entity_name)) {
    ldout(cct, 20) << "_check_auth_rotating not needed by " << entity_name << dendl;
    return 0;
  }

/** comment by hy 2020-01-16
 * # 没有已经认证了的连接
 */
  if (!active_con || !auth) {
    ldout(cct, 10) << "_check_auth_rotating waiting for auth session" << dendl;
    return 0;
  }

  utime_t now = ceph_clock_now();
  utime_t cutoff = now;
/** comment by hy 2020-01-16
 * # 至少30秒的间隔吧
 */
  cutoff -= std::min(30.0, cct->_conf->auth_service_ticket_ttl / 4.0);
  utime_t issued_at_lower_bound = now;
  issued_at_lower_bound -= cct->_conf->auth_service_ticket_ttl;
/** comment by hy 2020-01-16
 * # 证书小于3个,证书使用超过30秒了,就算都已经超时,需要更新证书
 */
  if (!rotating_secrets->need_new_secrets(cutoff)) {
    ldout(cct, 10) << "_check_auth_rotating have uptodate secrets (they expire after " << cutoff << ")" << dendl;
    rotating_secrets->dump_rotating();
    return 0;
  }

  ldout(cct, 10) << "_check_auth_rotating renewing rotating keys (they expired before " << cutoff << ")" << dendl;
/** comment by hy 2020-01-16
 * # 证书小于3个,且间隔时间已经超时需要更新证书
 */
  if (!rotating_secrets->need_new_secrets() &&
      rotating_secrets->need_new_secrets(issued_at_lower_bound)) {
    // the key has expired before it has been issued?
    lderr(cct) << __func__ << " possible clock skew, rotating keys expired way too early"
               << " (before " << issued_at_lower_bound << ")" << dendl;
  }
/** comment by hy 2020-01-16
 * # 间隔一秒以内就休息一下,别继续探测证书了
 */
  if ((now > last_rotating_renew_sent) &&
      double(now - last_rotating_renew_sent) < 1) {
    ldout(cct, 10) << __func__ << " called too often (last: "
                   << last_rotating_renew_sent << "), skipping refresh" << dendl;
    return 0;
  }
  auto m = ceph::make_message<MAuth>();
/** comment by hy 2020-01-16
 * # 开始发送认证,这里暂时build_rotating_request 暂时不会失败
        也许是为以后保留失败处理流程
 */
  m->protocol = auth->get_protocol();
  if (auth->build_rotating_request(m->auth_payload)) {
    last_rotating_renew_sent = now;
    _send_mon_message(std::move(m));
  }
  return 0;
}

int MonClient::wait_auth_rotating(double timeout)
{
  std::unique_lock l(monc_lock);

  // Must be initialized
  ceph_assert(auth != nullptr);

  if (auth->get_protocol() == CEPH_AUTH_NONE)
    return 0;
  
  if (!rotating_secrets)
    return 0;

  ldout(cct, 10) << __func__ << " waiting for " << timeout << dendl;
  utime_t now = ceph_clock_now();
  if (auth_cond.wait_for(l, ceph::make_timespan(timeout), [now, this] {
    return (!auth_principal_needs_rotating_keys(entity_name) ||
	    !rotating_secrets->need_new_secrets(now));
  })) {
    ldout(cct, 10) << __func__ << " done" << dendl;
    return 0;
  } else {
    ldout(cct, 0) << __func__ << " timed out after " << timeout << dendl;
    return -ETIMEDOUT;
  }
}

// ---------

void MonClient::_send_command(MonCommand *r)
{
  if (r->is_tell()) {
    ++r->send_attempts;
    if (r->send_attempts > cct->_conf->mon_client_directed_command_retry) {
      _finish_command(r, -ENXIO, "mon unavailable");
      return;
    }

    // tell-style command
    if (monmap.min_mon_release >= ceph_release_t::octopus) {
      if (r->target_con) {
	r->target_con->mark_down();
      }
      if (r->target_rank >= 0) {
	if (r->target_rank >= (int)monmap.size()) {
	  ldout(cct, 10) << " target " << r->target_rank
			 << " >= max mon " << monmap.size() << dendl;
	  _finish_command(r, -ENOENT, "mon rank dne");
	  return;
	}
	r->target_con = messenger->connect_to_mon(
	  monmap.get_addrs(r->target_rank), true /* anon */);
      } else {
	if (!monmap.contains(r->target_name)) {
	  ldout(cct, 10) << " target " << r->target_name
			 << " not present in monmap" << dendl;
	  _finish_command(r, -ENOENT, "mon dne");
	  return;
	}
	r->target_con = messenger->connect_to_mon(
	  monmap.get_addrs(r->target_name), true /* anon */);
      }

      r->target_session.reset(new MonConnection(cct, r->target_con, 0,
						&auth_registry));
      r->target_session->start(monmap.get_epoch(), entity_name);
      r->last_send_attempt = ceph_clock_now();

      MCommand *m = new MCommand(monmap.fsid);
      m->set_tid(r->tid);
      m->cmd = r->cmd;
      m->set_data(r->inbl);
      r->target_session->queue_command(m);
      return;
    }

    // ugly legacy handling of pre-octopus mons
    entity_addr_t peer;
    if (active_con) {
      peer = active_con->get_con()->get_peer_addr();
    }

    if (r->target_rank >= 0 &&
	r->target_rank != monmap.get_rank(peer)) {
      ldout(cct, 10) << __func__ << " " << r->tid << " " << r->cmd
		     << " wants rank " << r->target_rank
		     << ", reopening session"
		     << dendl;
      if (r->target_rank >= (int)monmap.size()) {
	ldout(cct, 10) << " target " << r->target_rank
		       << " >= max mon " << monmap.size() << dendl;
	_finish_command(r, -ENOENT, "mon rank dne");
	return;
      }
      _reopen_session(r->target_rank);
      return;
    }
    if (r->target_name.length() &&
	r->target_name != monmap.get_name(peer)) {
      ldout(cct, 10) << __func__ << " " << r->tid << " " << r->cmd
		     << " wants mon " << r->target_name
		     << ", reopening session"
		     << dendl;
      if (!monmap.contains(r->target_name)) {
	ldout(cct, 10) << " target " << r->target_name
		       << " not present in monmap" << dendl;
	_finish_command(r, -ENOENT, "mon dne");
	return;
      }
      _reopen_session(monmap.get_rank(r->target_name));
      return;
    }
    // fall-thru to send 'normal' CLI command
  }

  // normal CLI command
  ldout(cct, 10) << __func__ << " " << r->tid << " " << r->cmd << dendl;
  auto m = ceph::make_message<MMonCommand>(monmap.fsid);
  m->set_tid(r->tid);
  m->cmd = r->cmd;
  m->set_data(r->inbl);
  _send_mon_message(std::move(m));
  return;
}

void MonClient::_check_tell_commands()
{
  // resend any requests
  auto now = ceph_clock_now();
  auto p = mon_commands.begin();
  while (p != mon_commands.end()) {
    auto cmd = p->second;
    ++p;
    if (cmd->is_tell() &&
	cmd->last_send_attempt != utime_t() &&
	now - cmd->last_send_attempt > cct->_conf->mon_client_hunt_interval) {
      ldout(cct,5) << __func__ << " timeout tell command " << cmd->tid << dendl;
      _send_command(cmd); // might remove cmd from mon_commands
    }
  }
}

void MonClient::_resend_mon_commands()
{
  // resend any requests
  auto p = mon_commands.begin();
  while (p != mon_commands.end()) {
    auto cmd = p->second;
    ++p;
    if (cmd->is_tell() && monmap.min_mon_release >= ceph_release_t::octopus) {
      // starting with octopus, tell commands use their own connetion and need no
      // special resend when we finish hunting.
    } else {
      _send_command(cmd); // might remove cmd from mon_commands
    }
  }
}

void MonClient::handle_mon_command_ack(MMonCommandAck *ack)
{
  MonCommand *r = NULL;
  uint64_t tid = ack->get_tid();

  if (tid == 0 && !mon_commands.empty()) {
    r = mon_commands.begin()->second;
    ldout(cct, 10) << __func__ << " has tid 0, assuming it is " << r->tid << dendl;
  } else {
    auto p = mon_commands.find(tid);
    if (p == mon_commands.end()) {
      ldout(cct, 10) << __func__ << " " << ack->get_tid() << " not found" << dendl;
      ack->put();
      return;
    }
    r = p->second;
  }

  ldout(cct, 10) << __func__ << " " << r->tid << " " << r->cmd << dendl;
  if (r->poutbl)
    r->poutbl->claim(ack->get_data());
  _finish_command(r, ack->r, ack->rs);
  ack->put();
}

void MonClient::handle_command_reply(MCommandReply *reply)
{
  MonCommand *r = NULL;
  uint64_t tid = reply->get_tid();

  if (tid == 0 && !mon_commands.empty()) {
    r = mon_commands.begin()->second;
    ldout(cct, 10) << __func__ << " has tid 0, assuming it is " << r->tid
		   << dendl;
  } else {
    auto p = mon_commands.find(tid);
    if (p == mon_commands.end()) {
      ldout(cct, 10) << __func__ << " " << reply->get_tid() << " not found"
		     << dendl;
      reply->put();
      return;
    }
    r = p->second;
  }

  ldout(cct, 10) << __func__ << " " << r->tid << " " << r->cmd << dendl;
  if (r->poutbl)
    r->poutbl->claim(reply->get_data());
  _finish_command(r, reply->r, reply->rs);
  reply->put();
}

int MonClient::_cancel_mon_command(uint64_t tid)
{
  ceph_assert(ceph_mutex_is_locked(monc_lock));

  auto it = mon_commands.find(tid);
  if (it == mon_commands.end()) {
    ldout(cct, 10) << __func__ << " tid " << tid << " dne" << dendl;
    return -ENOENT;
  }

  ldout(cct, 10) << __func__ << " tid " << tid << dendl;

  MonCommand *cmd = it->second;
  _finish_command(cmd, -ETIMEDOUT, "");
  return 0;
}

void MonClient::_finish_command(MonCommand *r, int ret, string rs)
{
  ldout(cct, 10) << __func__ << " " << r->tid << " = " << ret << " " << rs << dendl;
  if (r->prval)
    *(r->prval) = ret;
  if (r->prs)
    *(r->prs) = rs;
  if (r->onfinish)
    finisher.queue(r->onfinish, ret);
  if (r->target_con) {
    r->target_con->mark_down();
  }
  mon_commands.erase(r->tid);
  delete r;
}

void MonClient::start_mon_command(const std::vector<string>& cmd,
                                  const ceph::buffer::list& inbl,
                                  ceph::buffer::list *outbl, string *outs,
                                  Context *onfinish)
{
  ldout(cct,10) << __func__ << " cmd=" << cmd << dendl;
  std::lock_guard l(monc_lock);
  if (!initialized || stopping) {
    if (onfinish) {
      onfinish->complete(-ECANCELED);
    }
    return;
  }
/** comment by hy 2020-03-20
 * # 数据包装
     最后通过 操作码 MSG_MON_COMMAND 发送消息
     由 Monitor::handle_tell_command 来处理
     Monitor::handle_command
 */
  MonCommand *r = new MonCommand(++last_mon_command_tid);
  r->cmd = cmd;
  r->inbl = inbl;
  r->poutbl = outbl;
  r->prs = outs;
  r->onfinish = onfinish;
  if (cct->_conf->rados_mon_op_timeout > 0) {
    class C_CancelMonCommand : public Context
    {
      uint64_t tid;
      MonClient *monc;
      public:
      C_CancelMonCommand(uint64_t tid, MonClient *monc) : tid(tid), monc(monc) {}
      void finish(int r) override {
	monc->_cancel_mon_command(tid);
      }
    };
    r->ontimeout = new C_CancelMonCommand(r->tid, this);
    timer.add_event_after(cct->_conf->rados_mon_op_timeout, r->ontimeout);
  }
  mon_commands[r->tid] = r;
/** comment by hy 2020-03-20
 * # 发送命令
 */
  _send_command(r);
}

void MonClient::start_mon_command(const string &mon_name,
                                  const std::vector<string>& cmd,
                                  const ceph::buffer::list& inbl,
                                  ceph::buffer::list *outbl, string *outs,
                                  Context *onfinish)
{
  ldout(cct,10) << __func__ << " mon." << mon_name << " cmd=" << cmd << dendl;
  std::lock_guard l(monc_lock);
  if (!initialized || stopping) {
    if (onfinish) {
      onfinish->complete(-ECANCELED);
    }
    return;
  }
  MonCommand *r = new MonCommand(++last_mon_command_tid);

  // detect/tolerate mon *rank* passed as a string
  string err;
  int rank = strict_strtoll(mon_name.c_str(), 10, &err);
  if (err.size() == 0 && rank >= 0) {
    ldout(cct,10) << __func__ << " interpreting name '" << mon_name
		  << "' as rank " << rank << dendl;
    r->target_rank = rank;
  } else {
    r->target_name = mon_name;
  }
  r->cmd = cmd;
  r->inbl = inbl;
  r->poutbl = outbl;
  r->prs = outs;
  r->onfinish = onfinish;
  mon_commands[r->tid] = r;
  _send_command(r);
}

void MonClient::start_mon_command(int rank,
                                  const std::vector<string>& cmd,
                                  const ceph::buffer::list& inbl,
                                  ceph::buffer::list *outbl, string *outs,
                                  Context *onfinish)
{
  ldout(cct,10) << __func__ << " rank " << rank << " cmd=" << cmd << dendl;
  std::lock_guard l(monc_lock);
  if (!initialized || stopping) {
    if (onfinish) {
      onfinish->complete(-ECANCELED);
    }
    return;
  }
  MonCommand *r = new MonCommand(++last_mon_command_tid);
  r->target_rank = rank;
  r->cmd = cmd;
  r->inbl = inbl;
  r->poutbl = outbl;
  r->prs = outs;
  r->onfinish = onfinish;
  mon_commands[r->tid] = r;
  _send_command(r);
}

// ---------

void MonClient::get_version(string map, version_t *newest, version_t *oldest, Context *onfinish)
{
  version_req_d *req = new version_req_d(onfinish, newest, oldest);
  ldout(cct, 10) << "get_version " << map << " req " << req << dendl;
  std::lock_guard l(monc_lock);
  auto m = ceph::make_message<MMonGetVersion>();
  m->what = map;
  m->handle = ++version_req_id;
  version_requests[m->handle] = req;
  _send_mon_message(std::move(m));
}

void MonClient::handle_get_version_reply(MMonGetVersionReply* m)
{
  ceph_assert(ceph_mutex_is_locked(monc_lock));
  auto iter = version_requests.find(m->handle);
  if (iter == version_requests.end()) {
    ldout(cct, 0) << __func__ << " version request with handle " << m->handle
		  << " not found" << dendl;
  } else {
    version_req_d *req = iter->second;
    ldout(cct, 10) << __func__ << " finishing " << req << " version " << m->version << dendl;
    version_requests.erase(iter);
    if (req->newest)
      *req->newest = m->version;
    if (req->oldest)
      *req->oldest = m->oldest_version;
    finisher.queue(req->context, 0);
    delete req;
  }
  m->put();
}

int MonClient::get_auth_request(
  Connection *con,
  AuthConnectionMeta *auth_meta,
  uint32_t *auth_method,
  std::vector<uint32_t> *preferred_modes,
  ceph::buffer::list *bl)
{
  std::lock_guard l(monc_lock);
  ldout(cct,10) << __func__ << " con " << con << " auth_method " << *auth_method
		<< dendl;

  // connection to mon?
  if (con->get_peer_type() == CEPH_ENTITY_TYPE_MON) {
    ceph_assert(!auth_meta->authorizer);
    if (con->is_anon()) {
      for (auto& i : mon_commands) {
	if (i.second->target_con == con) {
	  return i.second->target_session->get_auth_request(
	    auth_method, preferred_modes, bl,
	    entity_name, want_keys, rotating_secrets.get());
	}
      }
    }
    for (auto& i : pending_cons) {
      if (i.second.is_con(con)) {
	return i.second.get_auth_request(
	  auth_method, preferred_modes, bl,
	  entity_name, want_keys, rotating_secrets.get());
      }
    }
    return -ENOENT;
  }

  // generate authorizer
  if (!auth) {
    lderr(cct) << __func__ << " but no auth handler is set up" << dendl;
    return -EACCES;
  }
  auth_meta->authorizer.reset(auth->build_authorizer(con->get_peer_type()));
  if (!auth_meta->authorizer) {
    lderr(cct) << __func__ << " failed to build_authorizer for type "
	       << ceph_entity_type_name(con->get_peer_type()) << dendl;
    return -EACCES;
  }
  auth_meta->auth_method = auth_meta->authorizer->protocol;
  auth_registry.get_supported_modes(con->get_peer_type(),
				    auth_meta->auth_method,
				    preferred_modes);
  *bl = auth_meta->authorizer->bl;
  return 0;
}

int MonClient::handle_auth_reply_more(
  Connection *con,
  AuthConnectionMeta *auth_meta,
  const ceph::buffer::list& bl,
  ceph::buffer::list *reply)
{
  std::lock_guard l(monc_lock);

  if (con->get_peer_type() == CEPH_ENTITY_TYPE_MON) {
    if (con->is_anon()) {
      for (auto& i : mon_commands) {
	if (i.second->target_con == con) {
	  return i.second->target_session->handle_auth_reply_more(
	    auth_meta, bl, reply);
	}
      }
    }
    for (auto& i : pending_cons) {
      if (i.second.is_con(con)) {
	return i.second.handle_auth_reply_more(auth_meta, bl, reply);
      }
    }
    return -ENOENT;
  }

  // authorizer challenges
  if (!auth || !auth_meta->authorizer) {
    lderr(cct) << __func__ << " no authorizer?" << dendl;
    return -1;
  }
  auth_meta->authorizer->add_challenge(cct, bl);
  *reply = auth_meta->authorizer->bl;
  return 0;
}

int MonClient::handle_auth_done(
  Connection *con,
  AuthConnectionMeta *auth_meta,
  uint64_t global_id,
  uint32_t con_mode,
  const ceph::buffer::list& bl,
  CryptoKey *session_key,
  std::string *connection_secret)
{
  if (con->get_peer_type() == CEPH_ENTITY_TYPE_MON) {
    std::lock_guard l(monc_lock);
    if (con->is_anon()) {
      for (auto& i : mon_commands) {
	if (i.second->target_con == con) {
	  return i.second->target_session->handle_auth_done(
	    auth_meta, global_id, bl,
	    session_key, connection_secret);
	}
      }
    }
    for (auto& i : pending_cons) {
      if (i.second.is_con(con)) {
	int r = i.second.handle_auth_done(
	  auth_meta, global_id, bl,
	  session_key, connection_secret);
	if (r) {
	  pending_cons.erase(i.first);
	  if (!pending_cons.empty()) {
	    return r;
	  }
	} else {
	  active_con.reset(new MonConnection(std::move(i.second)));
	  pending_cons.clear();
	  ceph_assert(active_con->have_session());
	}

	_finish_hunting(r);
	if (r || monmap.get_epoch() > 0) {
	  _finish_auth(r);
	}
	return r;
      }
    }
    return -ENOENT;
  } else {
    // verify authorizer reply
    auto p = bl.begin();
    if (!auth_meta->authorizer->verify_reply(p, &auth_meta->connection_secret)) {
      ldout(cct, 0) << __func__ << " failed verifying authorizer reply"
		    << dendl;
      return -EACCES;
    }
    auth_meta->session_key = auth_meta->authorizer->session_key;
    return 0;
  }
}

int MonClient::handle_auth_bad_method(
  Connection *con,
  AuthConnectionMeta *auth_meta,
  uint32_t old_auth_method,
  int result,
  const std::vector<uint32_t>& allowed_methods,
  const std::vector<uint32_t>& allowed_modes)
{
  auth_meta->allowed_methods = allowed_methods;

  std::lock_guard l(monc_lock);
  if (con->get_peer_type() == CEPH_ENTITY_TYPE_MON) {
    if (con->is_anon()) {
      for (auto& i : mon_commands) {
	if (i.second->target_con == con) {
	  int r = i.second->target_session->handle_auth_bad_method(
	    old_auth_method,
	    result,
	    allowed_methods,
	    allowed_modes);
	  if (r < 0) {
	    _finish_command(i.second, r, "auth failed");
	  }
	  return r;
	}
      }
    }
    for (auto& i : pending_cons) {
      if (i.second.is_con(con)) {
	int r = i.second.handle_auth_bad_method(old_auth_method,
						result,
						allowed_methods,
						allowed_modes);
	if (r == 0) {
	  return r; // try another method on this con
	}
	pending_cons.erase(i.first);
	if (!pending_cons.empty()) {
	  return r;  // fail this con, maybe another con will succeed
	}
	// fail hunt
	_finish_hunting(r);
	_finish_auth(r);
	return r;
      }
    }
    return -ENOENT;
  } else {
    // huh...
    ldout(cct,10) << __func__ << " hmm, they didn't like " << old_auth_method
		  << " result " << cpp_strerror(result)
		  << " and auth is " << (auth ? auth->get_protocol() : 0)
		  << dendl;
    return -EACCES;
  }
}

int MonClient::handle_auth_request(
  Connection *con,
  AuthConnectionMeta *auth_meta,
  bool more,
  uint32_t auth_method,
  const ceph::buffer::list& payload,
  ceph::buffer::list *reply)
{
  if (payload.length() == 0) {
    // for some channels prior to nautilus (osd heartbeat), we
    // tolerate the lack of an authorizer.
    if (!con->get_messenger()->require_authorizer) {
      handle_authentication_dispatcher->ms_handle_authentication(con);
      return 1;
    }
    return -EACCES;
  }
  auth_meta->auth_mode = payload[0];
  if (auth_meta->auth_mode < AUTH_MODE_AUTHORIZER ||
      auth_meta->auth_mode > AUTH_MODE_AUTHORIZER_MAX) {
    return -EACCES;
  }
  AuthAuthorizeHandler *ah = get_auth_authorize_handler(con->get_peer_type(),
							auth_method);
  if (!ah) {
    lderr(cct) << __func__ << " no AuthAuthorizeHandler found for auth method "
	       << auth_method << dendl;
    return -EOPNOTSUPP;
  }

  auto ac = &auth_meta->authorizer_challenge;
  if (!HAVE_FEATURE(con->get_features(), CEPHX_V2)) {
    if (cct->_conf->cephx_service_require_version >= 2) {
      ldout(cct,10) << __func__ << " client missing CEPHX_V2 ("
		    << "cephx_service_requre_version = "
		    << cct->_conf->cephx_service_require_version << ")" << dendl;
      return -EACCES;
    }
    ac = nullptr;
  }

  bool was_challenge = (bool)auth_meta->authorizer_challenge;
  bool isvalid = ah->verify_authorizer(
    cct,
    *rotating_secrets,
    payload,
    auth_meta->get_connection_secret_length(),
    reply,
    &con->peer_name,
    &con->peer_global_id,
    &con->peer_caps_info,
    &auth_meta->session_key,
    &auth_meta->connection_secret,
    ac);
  if (isvalid) {
    handle_authentication_dispatcher->ms_handle_authentication(con);
    return 1;
  }
  if (!more && !was_challenge && auth_meta->authorizer_challenge) {
    ldout(cct,10) << __func__ << " added challenge on " << con << dendl;
    return 0;
  }
  ldout(cct,10) << __func__ << " bad authorizer on " << con << dendl;
  // discard old challenge
  auth_meta->authorizer_challenge.reset();
  return -EACCES;
}

AuthAuthorizer* MonClient::build_authorizer(int service_id) const {
  std::lock_guard l(monc_lock);
  if (auth) {
    return auth->build_authorizer(service_id);
  } else {
    ldout(cct, 0) << __func__ << " for " << ceph_entity_type_name(service_id)
		  << ", but no auth is available now" << dendl;
    return nullptr;
  }
}

#define dout_subsys ceph_subsys_monc
#undef dout_prefix
#define dout_prefix *_dout << "monclient" << (have_session() ? ": " : "(hunting): ")

MonConnection::MonConnection(
  CephContext *cct, ConnectionRef con, uint64_t global_id,
  AuthRegistry *ar)
  : cct(cct), con(con), global_id(global_id), auth_registry(ar)
{}

MonConnection::~MonConnection()
{
  if (con) {
    con->mark_down();
    con.reset();
  }
}

bool MonConnection::have_session() const
{
  return state == State::HAVE_SESSION;
}

void MonConnection::start(epoch_t epoch,
			  const EntityName& entity_name)
{
  using ceph::encode;
  auth_start = ceph_clock_now();

/** comment by hy 2020-01-16
 * # 如果本peer是管理,那么使用该连接发送获取monmap消息
 */
  if (con->get_peer_addr().is_msgr2()) {
    ldout(cct, 10) << __func__ << " opening mon connection" << dendl;
    state = State::AUTHENTICATING;
    con->send_message(new MMonGetMap());
    return;
  }

  // restart authentication handshake
  state = State::NEGOTIATING;

  // send an initial keepalive to ensure our timestamp is valid by the
  // time we are in an OPENED state (by sequencing this before
  // authentication).
/** comment by hy 2020-01-16
 * # 保持连接的keepalive
 */
  con->send_keepalive();

  auto m = new MAuth;
  m->protocol = CEPH_AUTH_UNKNOWN;
  m->monmap_epoch = epoch;
  __u8 struct_v = 1;
/** comment by hy 2020-01-16
 * # 消息加密组装,因为是想 public 网段发送所以加入安全考虑
 */
  encode(struct_v, m->auth_payload);
  std::vector<uint32_t> auth_supported;
  auth_registry->get_supported_methods(con->get_peer_type(), &auth_supported);
  encode(auth_supported, m->auth_payload);
  encode(entity_name, m->auth_payload);
  encode(global_id, m->auth_payload);
/** comment by hy 2020-01-16
 * # 发送认证消息
 */
  con->send_message(m);
}

int MonConnection::get_auth_request(
  uint32_t *method,
  std::vector<uint32_t> *preferred_modes,
  ceph::buffer::list *bl,
  const EntityName& entity_name,
  uint32_t want_keys,
  RotatingKeyRing* keyring)
{
  using ceph::encode;
  // choose method
  if (auth_method < 0) {
    std::vector<uint32_t> as;
    auth_registry->get_supported_methods(con->get_peer_type(), &as);
    if (as.empty()) {
      return -EACCES;
    }
    auth_method = as.front();
  }
  *method = auth_method;
  auth_registry->get_supported_modes(con->get_peer_type(), auth_method,
				     preferred_modes);
  ldout(cct,10) << __func__ << " method " << *method
		<< " preferred_modes " << *preferred_modes << dendl;
  if (preferred_modes->empty()) {
    return -EACCES;
  }

  if (auth) {
    auth.reset();
  }
  int r = _init_auth(*method, entity_name, want_keys, keyring, true);
  ceph_assert(r == 0);

  // initial requset includes some boilerplate...
  encode((char)AUTH_MODE_MON, *bl);
  encode(entity_name, *bl);
  encode(global_id, *bl);

  // and (maybe) some method-specific initial payload
  auth->build_initial_request(bl);

  return 0;
}

int MonConnection::handle_auth_reply_more(
  AuthConnectionMeta *auth_meta,
  const ceph::buffer::list& bl,
  ceph::buffer::list *reply)
{
  ldout(cct, 10) << __func__ << " payload " << bl.length() << dendl;
  ldout(cct, 30) << __func__ << " got\n";
  bl.hexdump(*_dout);
  *_dout << dendl;

  auto p = bl.cbegin();
  ldout(cct, 10) << __func__ << " payload_len " << bl.length() << dendl;
  int r = auth->handle_response(0, p, &auth_meta->session_key,
				&auth_meta->connection_secret);
  if (r == -EAGAIN) {
    auth->prepare_build_request();
    auth->build_request(*reply);
    ldout(cct, 10) << __func__ << " responding with " << reply->length()
		   << " bytes" << dendl;
    r = 0;
  } else if (r < 0) {
    lderr(cct) << __func__ << " handle_response returned " << r << dendl;
  } else {
    ldout(cct, 10) << __func__ << " authenticated!" << dendl;
    // FIXME
    ceph_abort(cct, "write me");
  }
  return r;
}

int MonConnection::handle_auth_done(
  AuthConnectionMeta *auth_meta,
  uint64_t new_global_id,
  const ceph::buffer::list& bl,
  CryptoKey *session_key,
  std::string *connection_secret)
{
  ldout(cct,10) << __func__ << " global_id " << new_global_id
		<< " payload " << bl.length()
		<< dendl;
  global_id = new_global_id;
  auth->set_global_id(global_id);
  auto p = bl.begin();
  int auth_err = auth->handle_response(0, p, &auth_meta->session_key,
				       &auth_meta->connection_secret);
  if (auth_err >= 0) {
    state = State::HAVE_SESSION;
  }
  con->set_last_keepalive_ack(auth_start);

  if (pending_tell_command) {
    con->send_message2(std::move(pending_tell_command));
  }
  return auth_err;
}

int MonConnection::handle_auth_bad_method(
  uint32_t old_auth_method,
  int result,
  const std::vector<uint32_t>& allowed_methods,
  const std::vector<uint32_t>& allowed_modes)
{
  ldout(cct,10) << __func__ << " old_auth_method " << old_auth_method
		<< " result " << cpp_strerror(result)
		<< " allowed_methods " << allowed_methods << dendl;
  std::vector<uint32_t> auth_supported;
  auth_registry->get_supported_methods(con->get_peer_type(), &auth_supported);
  auto p = std::find(auth_supported.begin(), auth_supported.end(),
		     old_auth_method);
  assert(p != auth_supported.end());
  p = std::find_first_of(std::next(p), auth_supported.end(),
			 allowed_methods.begin(), allowed_methods.end());
  if (p == auth_supported.end()) {
    lderr(cct) << __func__ << " server allowed_methods " << allowed_methods
	       << " but i only support " << auth_supported << dendl;
    return -EACCES;
  }
  auth_method = *p;
  ldout(cct,10) << __func__ << " will try " << auth_method << " next" << dendl;
  return 0;
}

int MonConnection::handle_auth(MAuthReply* m,
			       const EntityName& entity_name,
			       uint32_t want_keys,
			       RotatingKeyRing* keyring)
{
  if (state == State::NEGOTIATING) {
    int r = _negotiate(m, entity_name, want_keys, keyring);
    if (r) {
      return r;
    }
    state = State::AUTHENTICATING;
  }
  int r = authenticate(m);
  if (!r) {
    state = State::HAVE_SESSION;
  }
  return r;
}

int MonConnection::_negotiate(MAuthReply *m,
			      const EntityName& entity_name,
			      uint32_t want_keys,
			      RotatingKeyRing* keyring)
{
  if (auth && (int)m->protocol == auth->get_protocol()) {
    // good, negotiation completed
    auth->reset();
    return 0;
  }

  int r = _init_auth(m->protocol, entity_name, want_keys, keyring, false);
  if (r == -ENOTSUP) {
    if (m->result == -ENOTSUP) {
      ldout(cct, 10) << "none of our auth protocols are supported by the server"
		     << dendl;
    }
    return m->result;
  }
  return r;
}

int MonConnection::_init_auth(
  uint32_t method,
  const EntityName& entity_name,
  uint32_t want_keys,
  RotatingKeyRing* keyring,
  bool msgr2)
{
  ldout(cct,10) << __func__ << " method " << method << dendl;
  auth.reset(
    AuthClientHandler::create(cct, method, keyring));
  if (!auth) {
    ldout(cct, 10) << " no handler for protocol " << method << dendl;
    return -ENOTSUP;
  }

  // do not request MGR key unless the mon has the SERVER_KRAKEN
  // feature.  otherwise it will give us an auth error.  note that
  // we have to use the FEATUREMASK because pre-jewel the kraken
  // feature bit was used for something else.
  if (!msgr2 &&
      (want_keys & CEPH_ENTITY_TYPE_MGR) &&
      !(con->has_features(CEPH_FEATUREMASK_SERVER_KRAKEN))) {
    ldout(cct, 1) << __func__
		  << " not requesting MGR keys from pre-kraken monitor"
		  << dendl;
    want_keys &= ~CEPH_ENTITY_TYPE_MGR;
  }
  auth->set_want_keys(want_keys);
  auth->init(entity_name);
  auth->set_global_id(global_id);
  return 0;
}

int MonConnection::authenticate(MAuthReply *m)
{
  ceph_assert(auth);
  if (!m->global_id) {
    ldout(cct, 1) << "peer sent an invalid global_id" << dendl;
  }
  if (m->global_id != global_id) {
    // it's a new session
    auth->reset();
    global_id = m->global_id;
    auth->set_global_id(global_id);
    ldout(cct, 10) << "my global_id is " << m->global_id << dendl;
  }
  auto p = m->result_bl.cbegin();
  int ret = auth->handle_response(m->result, p, nullptr, nullptr);
  if (ret == -EAGAIN) {
    auto ma = new MAuth;
    ma->protocol = auth->get_protocol();
    auth->prepare_build_request();
    auth->build_request(ma->auth_payload);
    con->send_message(ma);
  }
  if (ret == 0 && pending_tell_command) {
    con->send_message2(std::move(pending_tell_command));
  }

  return ret;
}

void MonClient::register_config_callback(md_config_t::config_callback fn) {
  ceph_assert(!config_cb);
  config_cb = fn;
}

md_config_t::config_callback MonClient::get_config_callback() {
  return config_cb;
}
