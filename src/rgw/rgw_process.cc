// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab ft=cpp

#include "common/errno.h"
#include "common/Throttle.h"
#include "common/WorkQueue.h"
#include "include/scope_guard.h"

#include "rgw_rados.h"
#include "rgw_dmclock_scheduler.h"
#include "rgw_rest.h"
#include "rgw_frontend.h"
#include "rgw_request.h"
#include "rgw_process.h"
#include "rgw_loadgen.h"
#include "rgw_client_io.h"
#include "rgw_opa.h"
#include "rgw_perf_counters.h"

#include "services/svc_zone_utils.h"

#define dout_subsys ceph_subsys_rgw

using rgw::dmclock::Scheduler;

void RGWProcess::RGWWQ::_dump_queue()
{
  if (!g_conf()->subsys.should_gather<ceph_subsys_rgw, 20>()) {
    return;
  }
  deque<RGWRequest *>::iterator iter;
  if (process->m_req_queue.empty()) {
    dout(20) << "RGWWQ: empty" << dendl;
    return;
  }
  dout(20) << "RGWWQ:" << dendl;
  for (iter = process->m_req_queue.begin();
       iter != process->m_req_queue.end(); ++iter) {
    dout(20) << "req: " << hex << *iter << dec << dendl;
  }
} /* RGWProcess::RGWWQ::_dump_queue */

auto schedule_request(Scheduler *scheduler, req_state *s, RGWOp *op)
{
  using rgw::dmclock::SchedulerCompleter;
  if (!scheduler)
    return std::make_pair(0,SchedulerCompleter{});

  const auto client = op->dmclock_client();
  const auto cost = op->dmclock_cost();
  ldpp_dout(op,10) << "scheduling with dmclock client=" << static_cast<int>(client)
		   << " cost=" << cost << dendl;
  return scheduler->schedule_request(client, {},
                                     req_state::Clock::to_double(s->time),
                                     cost,
                                     s->yield);
}

bool RGWProcess::RGWWQ::_enqueue(RGWRequest* req) {
  process->m_req_queue.push_back(req);
  perfcounter->inc(l_rgw_qlen);
  dout(20) << "enqueued request req=" << hex << req << dec << dendl;
  _dump_queue();
  return true;
}

RGWRequest* RGWProcess::RGWWQ::_dequeue() {
  if (process->m_req_queue.empty())
    return NULL;
  RGWRequest *req = process->m_req_queue.front();
  process->m_req_queue.pop_front();
  dout(20) << "dequeued request req=" << hex << req << dec << dendl;
  _dump_queue();
  perfcounter->inc(l_rgw_qlen, -1);
  return req;
}

void RGWProcess::RGWWQ::_process(RGWRequest *req, ThreadPool::TPHandle &) {
  perfcounter->inc(l_rgw_qactive);
  process->handle_request(req);
  process->req_throttle.put(1);
  perfcounter->inc(l_rgw_qactive, -1);
}

int rgw_process_authenticated(RGWHandler_REST * const handler,
                              RGWOp *& op,
                              RGWRequest * const req,
                              req_state * const s,
                              const bool skip_retarget)
{
  ldpp_dout(op, 2) << "init permissions" << dendl;
/** comment by hy 2020-03-07
 * # 读取策略,以及头上IAM认证相关的信息
     对于创建bucket 创建
     RGWHandler_REST_Bucket_S3::init_permissions
     RGWHandler_REST::init_permissions
 */
  int ret = handler->init_permissions(op);
  if (ret < 0) {
    return ret;
  }

  /**
   * Only some accesses support website mode, and website mode does NOT apply
   * if you are using the REST endpoint either (ergo, no authenticated access)
   */
  if (! skip_retarget) {
    ldpp_dout(op, 2) << "recalculating target" << dendl;
/** comment by hy 2020-03-07
   # 对于创建bucket 创建
 * # RGWHandler_REST_Bucket_S3::retarget
     获得密钥
 */
    ret = handler->retarget(op, &op);
    if (ret < 0) {
      return ret;
    }
    req->op = op;
  } else {
    ldpp_dout(op, 2) << "retargeting skipped because of SubOp mode" << dendl;
  }

  /* If necessary extract object ACL and put them into req_state. */
  ldpp_dout(op, 2) << "reading permissions" << dendl;
/** comment by hy 2020-03-07
 * # 只有object 有ACL
 */
  ret = handler->read_permissions(op);
  if (ret < 0) {
    return ret;
  }

  ldpp_dout(op, 2) << "init op" << dendl;
/** comment by hy 2020-02-08
 * # 初始化相关限制
     用户,桶配额信息, 请求用户是否所有者用户
 */
  ret = op->init_processing();
  if (ret < 0) {
    return ret;
  }

  ldpp_dout(op, 2) << "verifying op mask" << dendl;
/** comment by hy 2020-03-07
 * # 读写的匹配
 */
  ret = op->verify_op_mask();
  if (ret < 0) {
    return ret;
  }

  /* Check if OPA is used to authorize requests */
  if (s->cct->_conf->rgw_use_opa_authz) {
/** comment by hy 2020-03-07
 * # OPA 认证
 */
    ret = rgw_opa_authorize(op, s);
    if (ret < 0) {
      return ret;
    }
  }

  ldpp_dout(op, 2) << "verifying op permissions" << dendl;
/** comment by hy 2020-03-07
 * # 操作权限
 */
  ret = op->verify_permission();
  if (ret < 0) {
    if (s->system_request) {
      dout(2) << "overriding permissions due to system operation" << dendl;
    } else if (s->auth.identity->is_admin_of(s->user->get_id())) {
      dout(2) << "overriding permissions due to admin operation" << dendl;
    } else {
      return ret;
    }
  }

  ldpp_dout(op, 2) << "verifying op params" << dendl;
/** comment by hy 2020-03-07
 * # 其他参数验证
 */
  ret = op->verify_params();
  if (ret < 0) {
    return ret;
  }

  ldpp_dout(op, 2) << "pre-executing" << dendl;
/** comment by hy 2020-02-08
 * # 作为模板方法,执行准备,执行,已经完成三个步骤
     每个操作对应方法
 */
/** comment by hy 2020-03-08
 * # 
     创建桶时
       RGWCreateBucket::pre_exec
       这个就是判断存在

    创建对象时
      RGWPutObjProcessor_Atomic::pre_exec =
      RGWPutObj:pre_exec
      生成对象名称前缀、设置placement rules
      在内存中创建对应的对象、设置切分head和tail对象的尺寸等等工作

    创建分片时
      RGWPutObjProcessor_Multipart::pre_exec =
      RGWPutObj:pre_exec
      比创建对象多了处理uploadId和partNumber的过程

      嵌套调用RGWPutObjProcessor_Aio的prepare
      根据用户配置，设置aio的window size
      然后会嵌套调用RGWPutObjProcessor的prepare
 */
  op->pre_exec();

  ldpp_dout(op, 2) << "executing" << dendl;
/** comment by hy 2020-03-08
     创建bucket时
 * # 写信息到 zone.root_pool
     RGWCreateBucket::execute

 * # 创建对象时
       RGWPutObjProcessor_Atomic::execute =
       RGWPutObj:execute
 
   # 创建分段时
       RGWPutObjProcessor_Multipart::execute =
        RGWPutObj:execute
        http://docs.ceph.com/docs/master/radosgw/s3/objectops/#initiate-multi-part-upload
 */
  op->execute();

  ldpp_dout(op, 2) << "completing" << dendl;
/** comment by hy 2020-03-15
 * # 创建桶时
       RGWCreateBucket::complete =
       RGWOp::complete
       开始发送响应

   # 创建对象时
       RGWPutObjProcessor_Atomic =    

   # 创建分片时
       RGWPutObjProcessor_Multipart =
 */
  op->complete();

  return 0;
}

int process_request(rgw::sal::RGWRadosStore* const store,
                    RGWREST* const rest,
                    RGWRequest* const req,
                    const std::string& frontend_prefix,
                    const rgw_auth_registry_t& auth_registry,
                    RGWRestfulIO* const client_io,
                    OpsLogSocket* const olog,
                    optional_yield yield,
		    rgw::dmclock::Scheduler *scheduler,
                    int* http_ret)
{
/** comment by hy 2020-02-08
   # 初始化客户端，主要是从request info中取出请求头来
     设置消息头 以及 连接信息
     RGWCivetWeb
 * # ConLenControllingFilter
     ChunkingFilter
     ReorderingFilter

     RGWCivetWeb::init_env
 */
  int ret = client_io->init(g_ceph_context);

  dout(1) << "====== starting new request req=" << hex << req << dec
	  << " =====" << dendl;
  perfcounter->inc(l_rgw_req);

/** comment by hy 2020-03-07
 * # 初始化执行环境,获取client_io的env来初始化
     获取根据请求包装后 env
     RGWCivetWeb::get_env
 */
  RGWEnv& rgw_env = client_io->get_env();

  rgw::sal::RGWRadosUser user;

/** comment by hy 2020-03-07
 * # 存储用于完成完成请求的所有信息
     生成请求状态空间
 */
  struct req_state rstate(g_ceph_context, &rgw_env, &user, req->id);
  struct req_state *s = &rstate;

/** comment by hy 2020-03-15
 * # 初始化rados上下文
     生成内容
 */
  RGWObjectCtx rados_ctx(store, s);
  s->obj_ctx = &rados_ctx;

/** comment by hy 2020-03-07
 * # RGWSI_SysObj::init_obj_ctx =
     生成 RGWSysObjectCtx 实例
 */
  auto sysobj_ctx = store->svc()->sysobj->init_obj_ctx();
  s->sysobj_ctx = &sysobj_ctx;

  if (ret < 0) {
    s->cio = client_io;
    abort_early(s, nullptr, ret, nullptr);
    return ret;
  }

/** comment by hy 2020-09-16
 * # 初始化请求跟踪等
 */
  s->req_id = store->svc()->zone_utils->unique_id(req->id);
  s->trans_id = store->svc()->zone_utils->unique_trans_id(req->id);
  s->host_id = store->getRados()->host_id;
  s->yield = yield;

  ldpp_dout(s, 2) << "initializing for trans_id = " << s->trans_id << dendl;

/** comment by hy 2020-09-16
 * # 初始化操作
 */
  RGWOp* op = nullptr;
  int init_error = 0;
  bool should_log = false;
  RGWRESTMgr *mgr;
/** comment by hy 2020-02-08
 * # 根据请求的url来选择对应的manager和该manager中的handler
     获取 url 解析出对应处理者
     RGWRESTMgr_S3::get_handler

     service 得到RGWHandler_REST类实例 如
       RGWHandler_REST_Service_S3

     bucket 得到RGWHandler_REST类实例 如
       RGWHandler_REST_Bucket_S3

     object 得到RGWHandler_REST类实例 如
       RGWHandler_REST_Obj_S3

       还有托管网页处理
 */
  RGWHandler_REST *handler = rest->get_handler(store, s,
                                               auth_registry,
                                               frontend_prefix,
                                               client_io, &mgr, &init_error);
  rgw::dmclock::SchedulerCompleter c;
  if (init_error != 0) {
    abort_early(s, nullptr, init_error, nullptr);
    goto done;
  }
  dout(10) << "handler=" << typeid(*handler).name() << dendl;

  should_log = mgr->get_logging();

  ldpp_dout(s, 2) << "getting op " << s->op << dendl;
/** comment by hy 2020-02-08
 * # 根据 request 的op得到操作处理
     获取调用对应的restful
     根据request的op得到对应的操作处理类 put/get/delete/post

     如 bucket 操作 put 默认方法
     RGWHandler_REST_Bucket_S3::get_op = 
     RGWHandler_REST_Bucket_S3::op_put = 
     RGWCreateBucket_ObjStore_S3

     如 object 操作 put 默认方法
     RGWHandler_REST_Obj_S3::get_op =
     RGWHandler_REST_Obj_S3::op_put = 
     RGWPutObj_ObjStore_S3
 */
  op = handler->get_op();
  if (!op) {
    abort_early(s, NULL, -ERR_METHOD_NOT_ALLOWED, handler);
    goto done;
  }
/** comment by hy 2020-03-07
 * # schedule 在网页框架初始化 是生成不同的实例
     RGWCivetWebFrontend 对应的是 dmc::SyncScheduler
     其基类
     rgw::dmclock::Scheduler
     对应 dmc::SyncScheduler::schedule_request
     dmc::SyncScheduler::schedule_request_impl
     实际 SyncScheduler::add_request

        放入的 scheduler 指定的队列中,是等待监控码
 */
  std::tie(ret,c) = schedule_request(scheduler, s, op);
  if (ret < 0) {
    if (ret == -EAGAIN) {
      ret = -ERR_RATE_LIMITED;
    }
    ldpp_dout(op,0) << "Scheduling request failed with " << ret << dendl;
    abort_early(s, op, ret, handler);
    goto done;
  }
  req->op = op;
  dout(10) << "op=" << typeid(*op).name() << dendl;

/** comment by hy 2020-03-07
 * # s3 api 类型
 */
  s->op_type = op->get_type();

  try {
    ldpp_dout(op, 2) << "verifying requester" << dendl;
/** comment by hy 2020-02-08
 * # 鉴合法,执行认证操作
     这里应该加载用户
 */
    ret = op->verify_requester(auth_registry);
    if (ret < 0) {
      dout(10) << "failed to authorize request" << dendl;
      abort_early(s, op, ret, handler);
      goto done;
    }

    /* FIXME: remove this after switching all handlers to the new authentication
     * infrastructure. */
    if (nullptr == s->auth.identity) {
      s->auth.identity = rgw::auth::transform_old_authinfo(s);
    }

    ldpp_dout(op, 2) << "normalizing buckets and tenants" << dendl;
/** comment by hy 2020-02-08
 * # 
     RGWHandler_REST_Bucket_S3::postauth_init = 
     RGWHandler_REST_Obj_S3::postauth_init =
     RGWHandler_REST_S3::postauth_init =
    调用 RGWHandler_REST_S3::postauth_init
     检查bucket以及object名字的有效性
 */
    ret = handler->postauth_init();
    if (ret < 0) {
      dout(10) << "failed to run post-auth init" << dendl;
      abort_early(s, op, ret, handler);
      goto done;
    }

/** comment by hy 2020-02-28
 * # 用户被禁用,加载到内存
 */
    if (s->user->get_info().suspended) {
      dout(10) << "user is suspended, uid=" << s->user->get_id() << dendl;
      abort_early(s, op, -ERR_USER_SUSPENDED, handler);
      goto done;
    }

/** comment by hy 2020-02-08
 * # 鉴定权限,并进行操作处理
     如果是创建bucket
       op = RGWCreateBucket_ObjStore_S3 =

        重要的是执行三个模板方法
        op->pre_exec();
        开始执行具体请求的操作
        op->execute();
        op->complete();

      创建桶操作对应的实体
        RGWCreateBucket_ObjStore_S3::pre_exec
        RGWCreateBucket_ObjStore_S3::execute
        RGWCreateBucket_ObjStore_S3::complete
         = RGWCreateBucket::pre_exec
         = RGWCreateBucket::execute
         这里会执行具体操作 如 RGWRados::create_bucket
         = RGWCreateBucket::complete = 注册的函数 用于通知

      上传对象 等对应操作
         = RGWPutObj_ObjStore_S3::pre_exec =
         RGWPutObj::pre_exec
         = RGWPutObj_ObjStore_S3::execute
         = RGWPutObj::execute
         = RGWPutObj_ObjStore_S3::complete = 注册的函数 用于通知
         = RGWPostObj_ObjStore_S3::pre_exec = 
         RGWPostObj::pre_exec
         = RGWPostObj_ObjStore_S3::execute =
         RGWPostObj::execute
         = RGWPostObj_ObjStore_S3::complete = 注册的函数 用于通知
 */
    ret = rgw_process_authenticated(handler, op, req, s);
    if (ret < 0) {
      abort_early(s, op, ret, handler);
      goto done;
    }
  } catch (const ceph::crypto::DigestException& e) {
    dout(0) << "authentication failed" << e.what() << dendl;
    abort_early(s, op, -ERR_INVALID_SECRET_KEY, handler);
  }

done:
/** comment by hy 2020-02-08
 * # 结束客户端请求
     收尾
     RGWCivetWeb::complete_request
 */
  try {
    client_io->complete_request();
  } catch (rgw::io::Exception& e) {
    dout(0) << "ERROR: client_io->complete_request() returned "
            << e.what() << dendl;
  }

  if (should_log) {
/** comment by hy 2020-03-07
 * # 记录到log pool 里
 */
    rgw_log_op(store->getRados(), rest, s, (op ? op->name() : "unknown"), olog);
  }

  if (http_ret != nullptr) {
    *http_ret = s->err.http_ret;
  }
  int op_ret = 0;
  if (op) {
    op_ret = op->get_ret();
    ldpp_dout(op, 2) << "op status=" << op_ret << dendl;
    ldpp_dout(op, 2) << "http status=" << s->err.http_ret << dendl;
  } else {
    ldpp_dout(s, 2) << "http status=" << s->err.http_ret << dendl;
  }
/** comment by hy 2020-02-08
 * # 回收op
 */
  if (handler)
    handler->put_op(op);
/** comment by hy 2020-02-08
 * # 回收 handle
 */
  rest->put_handler(handler);

  dout(1) << "====== req done req=" << hex << req << dec
	  << " op status=" << op_ret
	  << " http_status=" << s->err.http_ret
	  << " latency=" << s->time_elapsed()
	  << " ======"
	  << dendl;

  return (ret < 0 ? ret : s->err.ret);
} /* process_request */
