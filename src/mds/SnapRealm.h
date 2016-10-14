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

#ifndef CEPH_MDS_SNAPREALM_H
#define CEPH_MDS_SNAPREALM_H

#include "mdstypes.h"
#include "snap.h"
#include "include/xlist.h"
#include "include/elist.h"
#include "common/snap_types.h"


struct SnapRealm {
  // realm state
  sr_t srnode;

  // in-memory state
  MDCache *mdcache;
  CInode *inode;

  SnapRealm *parent;
  set<SnapRealm*> open_children;    // active children that are currently open
  set<SnapRealm*> open_past_children;  // past children who has pinned me

  // cache
  snapid_t cached_seq;           // max seq over self and all past+present parents.
  set<snapid_t> cached_snaps;
  SnapContext cached_snap_context;

  bufferlist cached_snap_trace;

  elist<CInode*> inodes_with_caps;             // for efficient realm splits
  map<client_t, xlist<Capability*>* > client_caps;   // to identify clients who need snap notifications

  SnapRealm(MDCache *c, CInode *in) : 
    srnode(),
    mdcache(c), inode(in),
    parent(0),
    inodes_with_caps(0) 
  { }

  bool exists(const string &name) const {
    for (map<snapid_t,SnapInfo>::const_iterator p = srnode.snaps.begin();
	 p != srnode.snaps.end();
	 ++p) {
      if (p->second.name == name)
	return true;
    }
    return false;
  }

  void prune_deleted_snaps();
  bool has_live_snapshots() { // this is what StrayManager REALLY cares about...
    return !srnode.snaps.empty();
  }

  void build_snap_set(set<snapid_t>& s, snapid_t& max_seq);
  void get_snap_info(map<snapid_t,SnapInfo*>& infomap, snapid_t first=0, snapid_t last=CEPH_NOSNAP);

  const bufferlist& get_snap_trace();
  void build_snap_trace(bufferlist& snapbl);

  snapid_t resolve_snapname(const string &name, inodeno_t atino, snapid_t first=0, snapid_t last=CEPH_NOSNAP);

  void check_cache();
  const set<snapid_t>& get_snaps();
  const SnapContext& get_snap_context();
  void invalidate_cached_snaps() {
    cached_seq = 0;
  }
  snapid_t get_last_created() {
    return srnode.last_created;
  }
  snapid_t get_last_destroyed() {
    return srnode.last_destroyed;
  }
  snapid_t get_newest_snap() {
    check_cache();
    if (cached_snaps.empty())
      return 0;
    else
      return *cached_snaps.rbegin();
  }
  snapid_t get_newest_seq() {
    check_cache();
    return cached_seq;
  }

  snapid_t get_snap_following(snapid_t follows) {
    check_cache();
    const set<snapid_t>& s = get_snaps();
    set<snapid_t>::const_iterator p = s.upper_bound(follows);
    if (p != s.end())
      return *p;
    return CEPH_NOSNAP;
  }

  bool has_snaps_in_range(snapid_t first, snapid_t last) {
    check_cache();
    const set<snapid_t>& s = get_snaps();
    set<snapid_t>::const_iterator p = s.lower_bound(first);
    return (p != s.end() && *p <= last);
  }

  void adjust_parent();

  void split_at(SnapRealm *child);

  void add_cap(client_t client, Capability *cap) {
    if (client_caps.count(client) == 0)
      client_caps[client] = new xlist<Capability*>;
    client_caps[client]->push_back(&cap->item_snaprealm_caps);
  }
  void remove_cap(client_t client, Capability *cap) {
    cap->item_snaprealm_caps.remove_myself();
    if (client_caps[client]->empty()) {
      delete client_caps[client];
      client_caps.erase(client);
    }
  }

  void merge_snaps_from(const SnapRealm *parent);
};

ostream& operator<<(ostream& out, const SnapRealm &realm);

#endif
