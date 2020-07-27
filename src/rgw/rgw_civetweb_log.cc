// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab ft=cpp

#include "common/config.h"
#include "rgw_common.h"

#include "civetweb/civetweb.h"
#include "rgw_crypt_sanitize.h"

#define dout_subsys ceph_subsys_civetweb


#define dout_context g_ceph_context
/*****************************************************************************
 * 函 数 名  : rgw_civetweb_log_callback
 * 负 责 人  : hy
 * 创建日期  : 2020年3月6日
 * 函数功能  : 通过 mg_cry 输出日志
 * 输入参数  : const struct mg_connection *conn  连接
               const char *buf                   日志内容
 * 输出参数  : 无
 * 返 回 值  : 
 * 调用关系  : 
 * 其    它  : 

*****************************************************************************/
int rgw_civetweb_log_callback(const struct mg_connection *conn, const char *buf) {
  dout(0) << "civetweb: " << (void *)conn << ": " << rgw::crypt_sanitize::log_content(buf) << dendl;
  return 0;
}

/*****************************************************************************
 * 函 数 名  : rgw_civetweb_log_access_callback
 * 负 责 人  : hy
 * 创建日期  : 2020年3月6日
 * 函数功能  : 有连接请求的回调
 * 输入参数  : const struct mg_connection *conn  连接
               const char *buf                   输出字符
 * 输出参数  : 无
 * 返 回 值  : 
 * 调用关系  : 
 * 其    它  : 

*****************************************************************************/
int rgw_civetweb_log_access_callback(const struct mg_connection *conn, const char *buf) {
  dout(1) << "civetweb: " << (void *)conn << ": " << rgw::crypt_sanitize::log_content(buf) << dendl;
  return 0;
}


