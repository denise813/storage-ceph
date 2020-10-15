// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include "msg/Message.h"
#include "mgr/ServiceMap.h"

class MServiceMap : public Message {
public:
  ServiceMap service_map;

  MServiceMap() : Message{MSG_SERVICE_MAP} { }
  explicit MServiceMap(const ServiceMap& sm)
    : Message{MSG_SERVICE_MAP},
      service_map(sm) {
  }
private:
  ~MServiceMap() override {}

public:
/* modify begin by hy, 2020-10-15, BugId:123 原因: */
  const ServiceMap & get_map() {return service_map;}
/* modify end by hy, 2020-10-15 */
  std::string_view get_type_name() const override { return "service_map"; }
  void print(ostream& out) const override {
    out << "service_map(e" << service_map.epoch << " "
	<< service_map.services.size() << " svc)";
  }
  void encode_payload(uint64_t features) override {
    using ceph::encode;
    encode(service_map, payload, features);
  }
  void decode_payload() override {
    auto p = payload.cbegin();
    decode(service_map, p);
  }
private:
/* modify begin by hy, 2020-10-15, BugId:123 原因: */
  using RefCountedObject::put;
  using RefCountedObject::get;
/* modify end by hy, 2020-10-15 */
  template<class T, typename... Args>
  friend boost::intrusive_ptr<T> ceph::make_message(Args&&... args);
};
