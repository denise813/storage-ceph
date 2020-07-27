// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
#ifndef CEPH_CLASSHANDLER_H
#define CEPH_CLASSHANDLER_H

#include <variant>

#include "include/types.h"
#include "include/common_fwd.h"
#include "common/ceph_mutex.h"
#include "objclass/objclass.h"

//forward declaration
class ClassHandler
{
public:
  CephContext *cct;
  struct ClassData;

  struct ClassMethod {
/** comment by hy 2020-02-18
 * # 方法名称
 */
    const std::string name;
    using func_t = std::variant<cls_method_cxx_call_t, cls_method_call_t>;
/** comment by hy 2020-02-18
 * # c/c++类型的函数指针
 */
    func_t func;
/** comment by hy 2020-02-18
 * # 方法相关标志
 */
    int flags = 0;
/** comment by hy 2020-02-18
 * # 所属模块信息
 */
    ClassData *cls = nullptr;

    int exec(cls_method_context_t ctx,
	     ceph::bufferlist& indata,
	     ceph::bufferlist& outdata);
    void unregister();

    int get_flags() {
      std::lock_guard l(cls->handler->mutex);
      return flags;
    }
    ClassMethod(const char* name, func_t call, int flags, ClassData* cls)
      : name{name}, func{call}, flags{flags}, cls{cls}
    {}
  };

  struct ClassFilter {
    ClassData *cls = nullptr;
    std::string name;
    cls_cxx_filter_factory_t fn = nullptr;

    void unregister();
  };

  struct ClassData {
/** comment by hy 2020-02-18
 * # 扩展模块状态
 */
    enum Status { 
/** comment by hy 2020-02-18
 * # 初始未知状态
 */
      CLASS_UNKNOWN,
/** comment by hy 2020-02-18
 * # 缺少状态(动态链接库找不到)
 */
      CLASS_MISSING,         // missing
/** comment by hy 2020-02-18
 * # 缺少依赖的模块
 */
      CLASS_MISSING_DEPS,    // missing dependencies
/** comment by hy 2020-02-18
 * # 正在初始化
 */
      CLASS_INITIALIZING,    // calling init() right now
/** comment by hy 2020-02-18
 * # 已经初始化,并加载
 */
      CLASS_OPEN,            // initialized, usable
    } status = CLASS_UNKNOWN;
/** comment by hy 2020-02-18
 * # 模块名称
 */
    std::string name;
/** comment by hy 2020-02-18
 * # 管理模块的加载指针
 */
    ClassHandler *handler = nullptr;
    void *handle = nullptr;

    bool whitelisted = false;
/** comment by hy 2020-02-18
 * # 模块下所有注册的方法
 */
    std::map<std::string, ClassMethod> methods_map;
/** comment by hy 2020-02-18
 * # 模块下所有注册过滤方法
 */
    std::map<std::string, ClassFilter> filters_map;
/** comment by hy 2020-02-18
 * # 本模块依赖的模块
 */
    std::set<ClassData *> dependencies;         /* our dependencies */
/** comment by hy 2020-02-18
 * # 缺少的依赖的模块
 */
    std::set<ClassData *> missing_dependencies; /* only missing dependencies */

    ClassMethod *_get_method(const std::string& mname);

    ClassMethod *register_method(const char *mname,
                                 int flags,
                                 cls_method_call_t func);
    ClassMethod *register_cxx_method(const char *mname,
                                     int flags,
                                     cls_method_cxx_call_t func);
    void unregister_method(ClassMethod *method);

    ClassFilter *register_cxx_filter(const std::string &filter_name,
                                     cls_cxx_filter_factory_t fn);
    void unregister_filter(ClassFilter *method);

    ClassMethod *get_method(const std::string& mname) {
      std::lock_guard l(handler->mutex);
      return _get_method(mname);
    }
    int get_method_flags(const std::string& mname);

    ClassFilter *get_filter(const std::string &filter_name) {
      std::lock_guard l(handler->mutex);
      if (auto i = filters_map.find(filter_name); i == filters_map.end()) {
        return nullptr;
      } else {
        return &(i->second);
      }
    }
  };

private:
/** comment by hy 2020-02-18
 * # 所有注册的模块,模块名->模块元数据信息
 */
  std::map<std::string, ClassData> classes;

  ClassData *_get_class(const std::string& cname, bool check_allowed);
  int _load_class(ClassData *cls);

  static bool in_class_list(const std::string& cname,
      const std::string& list);

  ceph::mutex mutex = ceph::make_mutex("ClassHandler");

public:
  explicit ClassHandler(CephContext *cct) : cct(cct) {}

  int open_all_classes();
  int open_class(const std::string& cname, ClassData **pcls);

  ClassData *register_class(const char *cname);
  void unregister_class(ClassData *cls);

  void shutdown();

  static ClassHandler& get_instance();
};


#endif
