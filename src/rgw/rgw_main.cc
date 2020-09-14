// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab ft=cpp

#include "common/ceph_argparse.h"
#include "global/global_init.h"
#include "global/signal_handler.h"
#include "common/config.h"
#include "common/errno.h"
#include "common/Timer.h"
#include "common/safe_io.h"
#include "common/TracepointProvider.h"
#include "common/numa.h"
#include "include/compat.h"
#include "include/str_list.h"
#include "include/stringify.h"
#include "rgw_common.h"
#include "rgw_rados.h"
#include "rgw_period_pusher.h"
#include "rgw_realm_reloader.h"
#include "rgw_rest.h"
#include "rgw_rest_s3.h"
#include "rgw_rest_swift.h"
#include "rgw_rest_admin.h"
#include "rgw_rest_usage.h"
#include "rgw_rest_user.h"
#include "rgw_rest_bucket.h"
#include "rgw_rest_metadata.h"
#include "rgw_rest_log.h"
#include "rgw_rest_config.h"
#include "rgw_rest_realm.h"
#include "rgw_rest_sts.h"
#include "rgw_swift_auth.h"
#include "rgw_log.h"
#include "rgw_tools.h"
#include "rgw_resolve.h"
#include "rgw_request.h"
#include "rgw_process.h"
#include "rgw_frontend.h"
#include "rgw_http_client_curl.h"
#include "rgw_perf_counters.h"
#ifdef WITH_RADOSGW_AMQP_ENDPOINT
#include "rgw_amqp.h"
#endif
#ifdef WITH_RADOSGW_KAFKA_ENDPOINT
#include "rgw_kafka.h"
#endif
#if defined(WITH_RADOSGW_BEAST_FRONTEND)
#include "rgw_asio_frontend.h"
#endif /* WITH_RADOSGW_BEAST_FRONTEND */
#include "rgw_dmclock_scheduler_ctx.h"

#include "services/svc_zone.h"

#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif

#define dout_subsys ceph_subsys_rgw

namespace {
TracepointProvider::Traits rgw_op_tracepoint_traits("librgw_op_tp.so",
                                                 "rgw_op_tracing");
TracepointProvider::Traits rgw_rados_tracepoint_traits("librgw_rados_tp.so",
                                                 "rgw_rados_tracing");
}

static sig_t sighandler_alrm;

class RGWProcess;

static int signal_fd[2] = {0, 0};

void signal_shutdown()
{
  int val = 0;
  int ret = write(signal_fd[0], (char *)&val, sizeof(val));
  if (ret < 0) {
    derr << "ERROR: " << __func__ << ": write() returned "
         << cpp_strerror(errno) << dendl;
  }
}

static void wait_shutdown()
{
  int val;
  int r = safe_read_exact(signal_fd[1], &val, sizeof(val));
  if (r < 0) {
    derr << "safe_read_exact returned with error" << dendl;
  }
}

static int signal_fd_init()
{
  return socketpair(AF_UNIX, SOCK_STREAM, 0, signal_fd);
}

static void signal_fd_finalize()
{
  close(signal_fd[0]);
  close(signal_fd[1]);
}

static void handle_sigterm(int signum)
{
  dout(1) << __func__ << dendl;
#if defined(WITH_RADOSGW_FCGI_FRONTEND)
  FCGX_ShutdownPending();
#endif

  // send a signal to make fcgi's accept(2) wake up.  unfortunately the
  // initial signal often isn't sufficient because we race with accept's
  // check of the flag wet by ShutdownPending() above.
  if (signum != SIGUSR1) {
    signal_shutdown();

    // safety net in case we get stuck doing an orderly shutdown.
    uint64_t secs = g_ceph_context->_conf->rgw_exit_timeout_secs;
    if (secs)
      alarm(secs);
    dout(1) << __func__ << " set alarm for " << secs << dendl;
  }

}

static void godown_alarm(int signum)
{
  _exit(0);
}


class C_InitTimeout : public Context {
public:
  C_InitTimeout() {}
  void finish(int r) override {
    derr << "Initialization timeout, failed to initialize" << dendl;
    exit(1);
  }
};

static int usage()
{
  cout << "usage: radosgw [options...]" << std::endl;
  cout << "options:\n";
  cout << "  --rgw-region=<region>     region in which radosgw runs\n";
  cout << "  --rgw-zone=<zone>         zone in which radosgw runs\n";
  cout << "  --rgw-socket-path=<path>  specify a unix domain socket path\n";
  cout << "  -m monaddress[:port]      connect to specified monitor\n";
  cout << "  --keyring=<path>          path to radosgw keyring\n";
  cout << "  --logfile=<logfile>       file to log debug output\n";
  cout << "  --debug-rgw=<log-level>/<memory-level>  set radosgw debug level\n";
  generic_server_usage();

  return 0;
}

static RGWRESTMgr *set_logging(RGWRESTMgr *mgr)
{
  mgr->set_logging(true);
  return mgr;
}

static RGWRESTMgr *rest_filter(RGWRados *store, int dialect, RGWRESTMgr *orig)
{
  RGWSyncModuleInstanceRef sync_module = store->get_sync_module();
  if (sync_module) {
    return sync_module->get_rest_filter(dialect, orig);
  } else {
    return orig;
  }
}

/*
 * start up the RADOS connection and then handle HTTP messages as they come in
 */
int radosgw_Main(int argc, const char **argv)
{
  // dout() messages will be sent to stderr, but FCGX wants messages on stdout
  // Redirect stderr to stdout.
  TEMP_FAILURE_RETRY(close(STDERR_FILENO));
  if (TEMP_FAILURE_RETRY(dup2(STDOUT_FILENO, STDERR_FILENO)) < 0) {
    int err = errno;
    cout << "failed to redirect stderr to stdout: " << cpp_strerror(err)
         << std::endl;
    return ENOSYS;
  }

  /* alternative default for module */
  map<string,string> defaults = {
    { "debug_rgw", "1/5" },
    { "keyring", "$rgw_data/keyring" },
    { "objecter_inflight_ops", "24576" }
  };

  vector<const char*> args;
  argv_to_vec(argc, argv, args);
  if (args.empty()) {
    cerr << argv[0] << ": -h or --help for usage" << std::endl;
    exit(1);
  }
  if (ceph_argparse_need_usage(args)) {
    usage();
    exit(0);
  }

  int flags = CINIT_FLAG_UNPRIVILEGED_DAEMON_DEFAULTS;
/** comment by hy 2019-12-21
 * # 初始化g_ceph_context、g_conf等全局变量
 */
  // Prevent global_init() from dropping permissions until frontends can bind
  // privileged ports
  flags |= CINIT_FLAG_DEFER_DROP_PRIVILEGES;

  auto cct = global_init(&defaults, args, CEPH_ENTITY_TYPE_CLIENT,
			 CODE_ENVIRONMENT_DAEMON,
			 flags, "rgw_data");

  // First, let's determine which frontends are configured.
  list<string> frontends;
  string rgw_frontends_str = g_conf().get_val<string>("rgw_frontends");
  g_conf().early_expand_meta(rgw_frontends_str, &cerr);
/** comment by hy 2020-03-03
 * # 根据配置文件加载前端
 */
  get_str_list(rgw_frontends_str, ",", frontends);
  multimap<string, RGWFrontendConfig *> fe_map;
  list<RGWFrontendConfig *> configs;
/** comment by hy 2020-01-07
 * # 设置默认的前端HTTP
 */
  if (frontends.empty()) {
    frontends.push_back("civetweb");
  }
  for (list<string>::iterator iter = frontends.begin(); iter != frontends.end(); ++iter) {
    string& f = *iter;

/** comment by hy 2020-01-08
 * # 检查配置文件中 http 服务的名称与端口号
     civetweb
     boost.beast 
 */
    if (f.find("civetweb") != string::npos || f.find("beast") != string::npos) {
      if (f.find("port") != string::npos) {
        // check for the most common ws problems
        if ((f.find("port=") == string::npos) ||
            (f.find("port= ") != string::npos)) {
          derr << "WARNING: radosgw frontend config found unexpected spacing around 'port' "
               << "(ensure frontend port parameter has the form 'port=80' with no spaces "
               << "before or after '=')" << dendl;
        }
      }
    }

/** comment by hy 2020-03-03
 * # 根据配置文件中的字符生成前端配置信息
 */
    RGWFrontendConfig *config = new RGWFrontendConfig(f);
/** comment by hy 2020-03-03
 * # 包装好配置信息内容
 */
    int r = config->init();
    if (r < 0) {
      delete config;
      cerr << "ERROR: failed to init config: " << f << std::endl;
      return EINVAL;
    }

    configs.push_back(config);

    string framework = config->get_framework();
    fe_map.insert(pair<string, RGWFrontendConfig*>(framework, config));
  }

  int numa_node = g_conf().get_val<int64_t>("rgw_numa_node");
  size_t numa_cpu_set_size = 0;
  cpu_set_t numa_cpu_set;

  if (numa_node >= 0) {
    int r = get_numa_node_cpu_set(numa_node, &numa_cpu_set_size, &numa_cpu_set);
    if (r < 0) {
      dout(1) << __func__ << " unable to determine rgw numa node " << numa_node
              << " CPUs" << dendl;
      numa_node = -1;
    } else {
      r = set_cpu_affinity_all_threads(numa_cpu_set_size, &numa_cpu_set);
      if (r < 0) {
        derr << __func__ << " failed to set numa affinity: " << cpp_strerror(r)
        << dendl;
      }
    }
  } else {
    dout(1) << __func__ << " not setting numa affinity" << dendl;
  }

  // maintain existing region root pool for new multisite objects
/** comment by hy 2020-03-03
 * # 读取root pool 默认的配置现象
 */
  if (!g_conf()->rgw_region_root_pool.empty()) {
    const char *root_pool = g_conf()->rgw_region_root_pool.c_str();
/** comment by hy 2020-03-03
 * # 根据root pool 设置 zone 信息 对应的 pool
     如果没设定root 就使用默认的 .rgw.root
 */
    if (g_conf()->rgw_zonegroup_root_pool.empty()) {
      g_conf().set_val_or_die("rgw_zonegroup_root_pool", root_pool);
    }
/** comment by hy 2020-03-03
 * # 根据root pool 设置 period 信息 对应的 pool
 */
    if (g_conf()->rgw_period_root_pool.empty()) {
      g_conf().set_val_or_die("rgw_period_root_pool", root_pool);
    }
/** comment by hy 2020-03-03
 * # 根据root pool 设置 realm 信息 对应的 pool
     这是用来进行隔离域
 */
    if (g_conf()->rgw_realm_root_pool.empty()) {
      g_conf().set_val_or_die("rgw_realm_root_pool", root_pool);
    }
  }

  // for region -> zonegroup conversion (must happen before common_init_finish())
  if (!g_conf()->rgw_region.empty() && g_conf()->rgw_zonegroup.empty()) {
/** comment by hy 2020-03-03
 * # 设置域
 */
    g_conf().set_val_or_die("rgw_zonegroup", g_conf()->rgw_region.c_str());
  }

  if (g_conf()->daemonize) {
    global_init_daemonize(g_ceph_context);
  }
/** comment by hy 2020-01-08
 * # 执行启动
 */
  ceph::mutex mutex = ceph::make_mutex("main");
  SafeTimer init_timer(g_ceph_context, mutex);
  init_timer.init();
/** comment by hy 2020-01-08
 * # 守护进程化时,避免竞争
 */
  mutex.lock();
  init_timer.add_event_after(g_conf()->rgw_init_timeout, new C_InitTimeout);
  mutex.unlock();

/** comment by hy 2019-12-21
 * # 启动admin socket线程,log模块线程
 */
  common_init_finish(g_ceph_context);

  init_async_signal_handler();
  register_async_signal_handler(SIGHUP, sighup_handler);

/** comment by hy 2020-01-08
 * # 检查库文件
 */
  TracepointProvider::initialize<rgw_rados_tracepoint_traits>(g_ceph_context);
  TracepointProvider::initialize<rgw_op_tracepoint_traits>(g_ceph_context);

/** comment by hy 2020-01-08
 * # mime_map 返回 Content-Type,即后缀解析使用
 */
  int r = rgw_tools_init(g_ceph_context);
  if (r < 0) {
    derr << "ERROR: unable to initialize rgw tools" << dendl;
    return -r;
  }

/** comment by hy 2019-12-27
 * # 准备加载域名解析服务
     生成域名解析服务实例 rgw_resolver
     这是一个单例
 */
  rgw_init_resolver();
/** comment by hy 2020-03-03
 * # 生成客户端用来发消息的handler
     handles = liburl.so
     单例
 */
  rgw::curl::setup_curl(fe_map);
/** comment by hy 2020-02-07
 * # 准备加载http客户端
     调用   curl_global_init
     生成 rgw_http_manager 单例
 */
  rgw_http_client_init(g_ceph_context);
  
#if defined(WITH_RADOSGW_FCGI_FRONTEND)
  FCGX_Init();
#endif

/** comment by hy 2020-02-07
 * # 创建对象存储引擎(rados客户端)
     默认情况下开启cache
     动态 resharding
     开启 gc
     开启 lifecycle
     开启配额控制
     必须开启 run_sync
 */
  rgw::sal::RGWRadosStore *store =
    RGWStoreManager::get_storage(g_ceph_context,
				 g_conf()->rgw_enable_gc_threads,
				 g_conf()->rgw_enable_lc_threads,
				 g_conf()->rgw_enable_quota_threads,
				 g_conf()->rgw_run_sync_thread,
				 g_conf().get_val<bool>("rgw_dynamic_resharding"),
				 g_conf()->rgw_cache_enabled);
  if (!store) {
    mutex.lock();
    init_timer.cancel_all_events();
    init_timer.shutdown();
    mutex.unlock();

    derr << "Couldn't init storage provider (RADOS)" << dendl;
    return EIO;
  }
/** comment by hy 2020-02-07
 * # 性能数据监控
 */
  r = rgw_perf_start(g_ceph_context);
  if (r < 0) {
    derr << "ERROR: failed starting rgw perf" << dendl;
    return -r;
  }

/** comment by hy 2020-02-07
 * # 准备rest请求解析服务
     常用http 头属性,hostname列表
 */
  rgw_rest_init(g_ceph_context, store->svc()->zone->get_zonegroup());

  mutex.lock();
/** comment by hy 2020-03-06
 * # 这个时候认为启动完成
 */
  init_timer.cancel_all_events();
  init_timer.shutdown();
  mutex.unlock();

/** comment by hy 2020-03-06
 * # 
 */
  rgw_log_usage_init(g_ceph_context, store->getRados());

  RGWREST rest;

  list<string> apis;

/** comment by hy 2020-02-07
 * # 获取服务支持的API
 */
  get_str_list(g_conf()->rgw_enable_apis, apis);

/** comment by hy 2020-02-07
 * # 生成接口列表
 */
  map<string, bool> apis_map;
  for (list<string>::iterator li = apis.begin(); li != apis.end(); ++li) {
    apis_map[*li] = true;
  }

  /* warn about insecure keystone secret config options */
  if (!(g_ceph_context->_conf->rgw_keystone_admin_token.empty() ||
	g_ceph_context->_conf->rgw_keystone_admin_password.empty())) {
    dout(0) << "WARNING: rgw_keystone_admin_token and rgw_keystone_admin_password should be avoided as they can expose secrets.  Prefer the new rgw_keystone_admin_token_path and rgw_keystone_admin_password_path options, which read their secrets from files." << dendl;
  }

  // S3 website mode is a specialization of S3
  const bool s3website_enabled = apis_map.count("s3website") > 0;
  const bool sts_enabled = apis_map.count("sts") > 0;
  const bool iam_enabled = apis_map.count("iam") > 0;
  const bool pubsub_enabled = apis_map.count("pubsub") > 0;
  // Swift API entrypoint could placed in the root instead of S3
/** comment by hy 2020-02-07
 * # swift API可能与s3的API造成冲突
 */
  const bool swift_at_root = g_conf()->rgw_swift_url_prefix == "/";
  if (apis_map.count("s3") > 0 || s3website_enabled) {
/** comment by hy 2020-02-07
 * # 注册s3restful,并设置为管理者
 */
    if (! swift_at_root) {
      rest.register_default_mgr(set_logging(rest_filter(store->getRados(), RGW_REST_S3,
                                                        new RGWRESTMgr_S3(s3website_enabled, sts_enabled, iam_enabled, pubsub_enabled))));
    } else {
      derr << "Cannot have the S3 or S3 Website enabled together with "
           << "Swift API placed in the root of hierarchy" << dendl;
      return EINVAL;
    }
  }

/** comment by hy 2020-03-14
 * # 消息队列
 */
  if (pubsub_enabled) {
#ifdef WITH_RADOSGW_AMQP_ENDPOINT
    if (!rgw::amqp::init(cct.get())) {
        dout(1) << "ERROR: failed to initialize AMQP manager" << dendl;
    }
#endif
#ifdef WITH_RADOSGW_KAFKA_ENDPOINT
    if (!rgw::kafka::init(cct.get())) {
        dout(1) << "ERROR: failed to initialize Kafka manager" << dendl;
    }
#endif
  }

/** comment by hy 2019-12-21
 * # 注册RGWREST类
 */
  if (apis_map.count("swift") > 0) {
    RGWRESTMgr_SWIFT* const swift_resource = new RGWRESTMgr_SWIFT;

    if (! g_conf()->rgw_cross_domain_policy.empty()) {
      swift_resource->register_resource("crossdomain.xml",
                          set_logging(new RGWRESTMgr_SWIFT_CrossDomain));
    }

    swift_resource->register_resource("healthcheck",
                          set_logging(new RGWRESTMgr_SWIFT_HealthCheck));

    swift_resource->register_resource("info",
                          set_logging(new RGWRESTMgr_SWIFT_Info));

    if (! swift_at_root) {
/** comment by hy 2020-02-07
 * # 注册 swift restful
 */
      rest.register_resource(g_conf()->rgw_swift_url_prefix,
                          set_logging(rest_filter(store->getRados(), RGW_REST_SWIFT,
                                                  swift_resource)));
    } else {
      if (store->svc()->zone->get_zonegroup().zones.size() > 1) {
        derr << "Placing Swift API in the root of URL hierarchy while running"
             << " multi-site configuration requires another instance of RadosGW"
             << " with S3 API enabled!" << dendl;
      }

/** comment by hy 2020-02-07
 * # 设置管理者
 */
      rest.register_default_mgr(set_logging(swift_resource));
    }
  }

  if (apis_map.count("swift_auth") > 0) {
    rest.register_resource(g_conf()->rgw_swift_auth_entry,
               set_logging(new RGWRESTMgr_SWIFT_Auth));
  }

/** comment by hy 2020-02-07
 * # 原生admin接口注册
 */
  if (apis_map.count("admin") > 0) {
    RGWRESTMgr_Admin *admin_resource = new RGWRESTMgr_Admin;
    admin_resource->register_resource("usage", new RGWRESTMgr_Usage);
    admin_resource->register_resource("user", new RGWRESTMgr_User);
    admin_resource->register_resource("bucket", new RGWRESTMgr_Bucket);

    /*Registering resource for /admin/metadata */
    admin_resource->register_resource("metadata", new RGWRESTMgr_Metadata);
    admin_resource->register_resource("log", new RGWRESTMgr_Log);
    admin_resource->register_resource("config", new RGWRESTMgr_Config);
    admin_resource->register_resource("realm", new RGWRESTMgr_Realm);
    rest.register_resource(g_conf()->rgw_admin_entry, admin_resource);
  }

  /* Initialize the registry of auth strategies which will coordinate
   * the dynamic reconfiguration. */
  rgw::auth::ImplicitTenants implicit_tenant_context{g_conf()};
  g_conf().add_observer(&implicit_tenant_context);
  auto auth_registry = \
    rgw::auth::StrategyRegistry::create(g_ceph_context, implicit_tenant_context, store->getRados()->pctl);

  /* Header custom behavior */
/** comment by hy 2020-02-07
 * # 监控操作日志
 */
  rest.register_x_headers(g_conf()->rgw_log_http_headers);

  if (cct->_conf.get_val<std::string>("rgw_scheduler_type") == "dmclock" &&
      !cct->check_experimental_feature_enabled("dmclock")){
    derr << "dmclock scheduler type is experimental and needs to be"
	 << "set in the option enable experimental data corrupting features"
	 << dendl;
    return EINVAL;
  }

  rgw::dmclock::SchedulerCtx sched_ctx{cct.get()};

  OpsLogSocket *olog = NULL;

  if (!g_conf()->rgw_ops_log_socket_path.empty()) {
    olog = new OpsLogSocket(g_ceph_context, g_conf()->rgw_ops_log_data_backlog);
    olog->init(g_conf()->rgw_ops_log_socket_path);
  }

  r = signal_fd_init();
  if (r < 0) {
    derr << "ERROR: unable to initialize signal fds" << dendl;
    exit(1);
  }

  register_async_signal_handler(SIGTERM, handle_sigterm);
  register_async_signal_handler(SIGINT, handle_sigterm);
  register_async_signal_handler(SIGUSR1, handle_sigterm);
  sighandler_alrm = signal(SIGALRM, godown_alarm);

  map<string, string> service_map_meta;
  service_map_meta["pid"] = stringify(getpid());

  list<RGWFrontend *> fes;

  string frontend_defs_str = g_conf().get_val<string>("rgw_frontend_defaults");

  list<string> frontends_def;
  get_str_list(frontend_defs_str, ",", frontends_def);

  map<string, std::unique_ptr<RGWFrontendConfig> > fe_def_map;
  for (auto& f : frontends_def) {
    RGWFrontendConfig *config = new RGWFrontendConfig(f);
    int r = config->init();
    if (r < 0) {
      delete config;
      cerr << "ERROR: failed to init default config: " << f << std::endl;
      return EINVAL;
    }

    fe_def_map[config->get_framework()].reset(config);
  }

  int fe_count = 0;
/** comment by hy 2020-02-07
 * # httpd服务准备
 */
  for (multimap<string, RGWFrontendConfig *>::iterator fiter = fe_map.begin();
       fiter != fe_map.end(); ++fiter, ++fe_count) {
    RGWFrontendConfig *config = fiter->second;
    string framework = config->get_framework();

    auto def_iter = fe_def_map.find(framework);
    if (def_iter != fe_def_map.end()) {
      config->set_default_config(*def_iter->second);
    }

    RGWFrontend *fe = NULL;

/** comment by hy 2020-03-17
 * # boost mongoose 库 据说性能不是很好
 */
    if (framework == "civetweb" || framework == "mongoose") {
      framework = "civetweb";
      std::string uri_prefix;
      config->get_val("prefix", "", &uri_prefix);

/** comment by hy 2020-02-07
 * # 创建上下文
 */
      RGWProcessEnv env = { store, &rest, olog, 0, uri_prefix, auth_registry };
      //TODO: move all of scheduler initializations to frontends?

      fe = new RGWCivetWebFrontend(env, config, sched_ctx);
    }
    else if (framework == "loadgen") {
      int port;
      config->get_val("port", 80, &port);
      std::string uri_prefix;
      config->get_val("prefix", "", &uri_prefix);

      RGWProcessEnv env = { store, &rest, olog, port, uri_prefix, auth_registry };

      fe = new RGWLoadGenFrontend(env, config);
    }
#if defined(WITH_RADOSGW_BEAST_FRONTEND)
    else if (framework == "beast") {
/** comment by hy 2020-03-06
 * # boost 的 HTTP parsing
     Boost.Asio 异步io 处理
     这个库这里开启了协程处理
     https://access.redhat.com/documentation/en-us/red_hat_ceph_storage/4/html-
     single/object_gateway_configuration_and_administration_guide/index#using-the-
     beast-front-end-rgw
 */
      int port;
      config->get_val("port", 80, &port);
      std::string uri_prefix;
      config->get_val("prefix", "", &uri_prefix);
      RGWProcessEnv env{ store, &rest, olog, port, uri_prefix, auth_registry };
      fe = new RGWAsioFrontend(env, config, sched_ctx);
    }
#endif /* WITH_RADOSGW_BEAST_FRONTEND */
#if defined(WITH_RADOSGW_FCGI_FRONTEND)
/** comment by hy 2020-03-11
 * # 用了其他前端,进入阿帕奇,nigix
 */
    else if (framework == "fastcgi" || framework == "fcgi") {
      framework = "fastcgi";
      std::string uri_prefix;
      config->get_val("prefix", "", &uri_prefix);
      RGWProcessEnv fcgi_pe = { store, &rest, olog, 0, uri_prefix, auth_registry };

      fe = new RGWFCGXFrontend(fcgi_pe, config);
    }
#endif /* WITH_RADOSGW_FCGI_FRONTEND */

    service_map_meta["frontend_type#" + stringify(fe_count)] = framework;
    service_map_meta["frontend_config#" + stringify(fe_count)] = config->get_config();

    if (fe == NULL) {
      dout(0) << "WARNING: skipping unknown framework: " << framework << dendl;
      continue;
    }

    dout(0) << "starting handler: " << fiter->first << dendl;
/** comment by hy 2020-02-07
 * # 执行准备工作
     RGWAsioFrontend::init = AsioFrontend::init
        进行异步监听端口等
     RGWCivetWebFrontend::init nothing to do
 */
    int r = fe->init();
    if (r < 0) {
      derr << "ERROR: failed initializing frontend" << dendl;
      return -r;
    }
/** comment by hy 2020-02-07
 * # 准备接受处理工作
     如以下两个
     RGWAsioFrontend::run
        = AsioFrontend::run
        最终根据配置文件线程数启动 boost::asio::io_service.run 启动

     RGWCivetWebFrontend::run
         注册请求对应的回调
           cb.begin_request = civetweb_callback;
           通过框架连接有请求的回调 派发消息
           z最后调用 process_request 处理请求
           cb.log_message = rgw_civetweb_log_callback;
           通过 调用mg_cry回调 记录消息日志
           cb.log_access = rgw_civetweb_log_access_callback;
           通过框架有连接请求的回调 输出访问日志
 */
    r = fe->run();
    if (r < 0) {
      derr << "ERROR: failed run" << dendl;
      return -r;
    }

    fes.push_back(fe);
  }

/** comment by hy 2020-02-07
 * # 加入到服务表等待MGR 服务监控
 */
  r = store->getRados()->register_to_service_map("rgw", service_map_meta);
  if (r < 0) {
    derr << "ERROR: failed to register to service map: " << cpp_strerror(-r) << dendl;

    /* ignore error */
  }


  // add a watcher to respond to realm configuration changes
  RGWPeriodPusher pusher(store);
  RGWFrontendPauser pauser(fes, implicit_tenant_context, &pusher);
  RGWRealmReloader reloader(store, service_map_meta, &pauser);

  RGWRealmWatcher realm_watcher(g_ceph_context, store->svc()->zone->get_realm());
  realm_watcher.add_watcher(RGWRealmNotify::Reload, reloader);
  realm_watcher.add_watcher(RGWRealmNotify::ZonesNeedPeriod, pusher);

#if defined(HAVE_SYS_PRCTL_H)
  if (prctl(PR_SET_DUMPABLE, 1) == -1) {
    cerr << "warning: unable to set dumpable flag: " << cpp_strerror(errno) << std::endl;
  }
#endif

  wait_shutdown();

/** comment by hy 2019-12-22
 * # 退出流程
 */
  derr << "shutting down" << dendl;

  for (list<RGWFrontend *>::iterator liter = fes.begin(); liter != fes.end();
       ++liter) {
    RGWFrontend *fe = *liter;
    fe->stop();
  }

  for (list<RGWFrontend *>::iterator liter = fes.begin(); liter != fes.end();
       ++liter) {
    RGWFrontend *fe = *liter;
    fe->join();
    delete fe;
  }

  for (list<RGWFrontendConfig *>::iterator liter = configs.begin();
       liter != configs.end(); ++liter) {
    RGWFrontendConfig *fec = *liter;
    delete fec;
  }

  unregister_async_signal_handler(SIGHUP, sighup_handler);
  unregister_async_signal_handler(SIGTERM, handle_sigterm);
  unregister_async_signal_handler(SIGINT, handle_sigterm);
  unregister_async_signal_handler(SIGUSR1, handle_sigterm);
  shutdown_async_signal_handler();

  rgw_log_usage_finalize();

  delete olog;

  RGWStoreManager::close_storage(store);
  rgw::auth::s3::LDAPEngine::shutdown();
  rgw_tools_cleanup();
  rgw_shutdown_resolver();
  rgw_http_client_cleanup();
  rgw::curl::cleanup_curl();
  g_conf().remove_observer(&implicit_tenant_context);
#ifdef WITH_RADOSGW_AMQP_ENDPOINT
  rgw::amqp::shutdown();
#endif
#ifdef WITH_RADOSGW_KAFKA_ENDPOINT
  rgw::kafka::shutdown();
#endif

  rgw_perf_stop(g_ceph_context);

  dout(1) << "final shutdown" << dendl;

  signal_fd_finalize();

  return 0;
}

extern "C" {

int radosgw_main(int argc, const char** argv)
{
  return radosgw_Main(argc, argv);
}

} /* extern "C" */

