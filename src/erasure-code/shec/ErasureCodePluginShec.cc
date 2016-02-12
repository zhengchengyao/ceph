// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2014 FUJITSU LIMITED
 * Copyright (C) 2013,2014 Cloudwatt <libre.licensing@cloudwatt.com>
 * Copyright (C) 2014 Red Hat <contact@redhat.com>
 *
 * Author: Takanori Nakao <nakao.takanori@jp.fujitsu.com>
 * Author: Takeshi Miyamae <miyamae.takeshi@jp.fujitsu.com>
 * Author: Loic Dachary <loic@dachary.org>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 */

#include "ceph_ver.h"
#include "common/debug.h"
#include "ErasureCodeShecTableCache.h"
#include "erasure-code/ErasureCodePlugin.h"
#include "ErasureCodeShec.h"
#include "jerasure_init.h"

#define dout_subsys ceph_subsys_osd
#undef dout_prefix
#define dout_prefix _prefix(_dout)

static ostream& _prefix(std::ostream* _dout)
{
  return *_dout << "ErasureCodePluginShec: ";
}

class ErasureCodePluginShec : public ErasureCodePlugin {
public:
  ErasureCodeShecTableCache tcache;

  ErasureCodePluginShec(CephContext* cct) : ErasureCodePlugin(cct)
  {}

  virtual int factory(ErasureCodeProfile &profile,
		      ErasureCodeInterfaceRef *erasure_code,
		      ostream *ss) {
    ErasureCodeShec *interface;

    if (profile.find("technique") == profile.end())
      profile["technique"] = "multiple";
    std::string t = profile.find("technique")->second;

    if (t == "single"){
      interface = new ErasureCodeShecReedSolomonVandermonde(tcache, ErasureCodeShec::SINGLE);
    } else if (t == "multiple"){
      interface = new ErasureCodeShecReedSolomonVandermonde(tcache, ErasureCodeShec::MULTIPLE);
    } else {
      *ss << "technique=" << t << " is not a valid coding technique. "
	  << "Choose one of the following: "
	  << "single, multiple ";
      return -ENOENT;
    }
    int r = interface->init(profile, ss);
    if (r) {
      delete interface;
      return r;
    }
    *erasure_code = ErasureCodeInterfaceRef(interface);

    dout(10) << "ErasureCodePluginShec: factory() completed" << dendl;

    return 0;
  }
};

const char *__ceph_plugin_version() { return CEPH_GIT_NICE_VER; }

int __ceph_plugin_init(CephContext *cct,
                       const std::string& type,
                       const std::string& name)
{
  PluginRegistry *instance = cct->get_plugin_registry();
  int w[] = { 8, 16, 32 };
  int r = jerasure_init(3, w);
  if (r) {
    return -r;
  }
  return instance->add(type, name, new ErasureCodePluginShec(cct));
}
