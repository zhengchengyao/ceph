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

#include "SnapRealm.h"
#include "MDCache.h"
#include "MDSRank.h"

#include "messages/MClientSnap.h"


/*
 * SnapRealm
 */

#define dout_subsys ceph_subsys_mds
#undef dout_prefix
#define dout_prefix _prefix(_dout, mdcache->mds->get_nodeid(), inode, srnode.seq, this)
static ostream& _prefix(std::ostream *_dout, int whoami, CInode *inode,
			uint64_t seq, SnapRealm *realm) {
  return *_dout << " mds." << whoami
		<< ".cache.snaprealm(" << inode->ino()
		<< " seq " << seq << " " << realm << ") ";
}

ostream& operator<<(ostream& out, const SnapRealm& realm) 
{
  out << "snaprealm(" << realm.inode->ino()
      << " seq " << realm.srnode.seq
      << " lc " << realm.srnode.last_created
      << " cr " << realm.srnode.created;
  if (realm.srnode.created != realm.srnode.current_parent_since)
    out << " cps " << realm.srnode.current_parent_since;
  out << " snaps=" << realm.srnode.snaps;
  if (realm.srnode.past_parents.size()) {
    out << " past_parents=(";
    for (map<snapid_t, snaplink_t>::const_iterator p = realm.srnode.past_parents.begin(); 
	 p != realm.srnode.past_parents.end(); 
	 ++p) {
      if (p != realm.srnode.past_parents.begin()) out << ",";
      out << p->second.first << "-" << p->first
	  << "=" << p->second.ino;
    }
    out << ")";
  }
  out << " " << &realm << ")";
  return out;
}


bool SnapRealm::_open_parents(MDSInternalContextBase *finish, snapid_t first, snapid_t last)
{
  dout(10) << "open_parents [" << first << "," << last << "]" << dendl;
  if (open)
    return true;

  // make sure my current parents' parents are open...
  // TODO: Should be able to assume this, right? In which case knock out the
  // whole "open" concept entirely
  if (parent) {
    dout(10) << " current parent [" << srnode.current_parent_since << ",head] is " << *parent
	     << " on " << *parent->inode << dendl;
    if (last >= srnode.current_parent_since &&
	!parent->_open_parents(finish, MAX(first, srnode.current_parent_since), last))
      return false;
  }
  open = true;
  return true;
}

void SnapRealm::close_parents()
{
  // TODO: Zap this function completely
  return;
}


/*
 * get list of snaps for this realm.  we must include parents' snaps
 * for the intervals during which they were our parent.
 */
void SnapRealm::build_snap_set(set<snapid_t> &s,
			       snapid_t& max_seq, snapid_t& max_last_created,
			       snapid_t first, snapid_t last)
{
  dout(10) << "build_snap_set [" << first << "," << last << "] on " << *this << dendl;

  if (srnode.seq > max_seq)
    max_seq = srnode.seq;
  if (srnode.last_created > max_last_created)
    max_last_created = srnode.last_created;

  // include my snaps within interval [first,last]
  for (map<snapid_t, SnapInfo>::iterator p = srnode.snaps.lower_bound(first); // first element >= first
       p != srnode.snaps.end() && p->first <= last;
       ++p)
    s.insert(p->first);
}


void SnapRealm::check_cache()
{
  assert(open);
  if (cached_seq >= srnode.seq)
    return;

  cached_snaps.clear();
  cached_snap_context.clear();

  cached_last_created = srnode.last_created;
  cached_seq = srnode.seq;
  build_snap_set(cached_snaps, cached_seq, cached_last_created,
		 0, CEPH_NOSNAP);

  cached_snap_trace.clear();
  build_snap_trace(cached_snap_trace);
  
  dout(10) << "check_cache rebuilt " << cached_snaps
	   << " seq " << srnode.seq
	   << " cached_seq " << cached_seq
	   << " cached_last_created " << cached_last_created
	   << ")" << dendl;
}

const set<snapid_t>& SnapRealm::get_snaps()
{
  check_cache();
  dout(10) << "get_snaps " << cached_snaps
	   << " (seq " << srnode.seq << " cached_seq " << cached_seq << ")"
	   << dendl;
  return cached_snaps;
}

/*
 * build vector in reverse sorted order
 */
const SnapContext& SnapRealm::get_snap_context()
{
  check_cache();

  if (!cached_snap_context.seq) {
    cached_snap_context.seq = cached_seq;
    cached_snap_context.snaps.resize(cached_snaps.size());
    unsigned i = 0;
    for (set<snapid_t>::reverse_iterator p = cached_snaps.rbegin();
	 p != cached_snaps.rend();
	 ++p)
      cached_snap_context.snaps[i++] = *p;
  }

  return cached_snap_context;
}

void SnapRealm::get_snap_info(map<snapid_t,SnapInfo*>& infomap, snapid_t first, snapid_t last)
{
  const set<snapid_t>& snaps = get_snaps();
  dout(10) << "get_snap_info snaps " << snaps << dendl;

  // include my snaps within interval [first,last]
  for (map<snapid_t, SnapInfo>::iterator p = srnode.snaps.lower_bound(first); // first element >= first
       p != srnode.snaps.end() && p->first <= last;
       ++p)
    infomap[p->first] = &p->second;

  // include snaps for parents during intervals that intersect [first,last]
  if (srnode.current_parent_since <= last && parent)
    parent->get_snap_info(infomap, MAX(first, srnode.current_parent_since), last);
}

snapid_t SnapRealm::resolve_snapname(const string& n, inodeno_t atino, snapid_t first, snapid_t last)
{
  // first try me
  dout(10) << "resolve_snapname '" << n << "' in [" << first << "," << last << "]" << dendl;

  //snapid_t num;
  //if (n[0] == '~') num = atoll(n.c_str()+1);

  bool actual = (atino == inode->ino());
  string pname;
  inodeno_t pino;
  if (!actual) {
    if (!n.length() ||
	n[0] != '_') return 0;
    int next_ = n.find('_', 1);
    if (next_ < 0) return 0;
    pname = n.substr(1, next_ - 1);
    pino = atoll(n.c_str() + next_ + 1);
    dout(10) << " " << n << " parses to name '" << pname << "' dirino " << pino << dendl;
  }

  for (map<snapid_t, SnapInfo>::iterator p = srnode.snaps.lower_bound(first); // first element >= first
       p != srnode.snaps.end() && p->first <= last;
       ++p) {
    dout(15) << " ? " << p->second << dendl;
    //if (num && p->second.snapid == num)
    //return p->first;
    if (actual && p->second.name == n)
	return p->first;
    if (!actual && p->second.name == pname && p->second.ino == pino)
      return p->first;
  }
  return 0;
}


void SnapRealm::adjust_parent()
{
  SnapRealm *newparent = inode->get_parent_dn()->get_dir()->get_inode()->find_snaprealm();
  if (newparent != parent) {
    dout(10) << "adjust_parent " << parent << " -> " << newparent << dendl;
    if (parent)
      parent->open_children.erase(this);
    parent = newparent;
    if (parent)
      parent->open_children.insert(this);
    
    invalidate_cached_snaps();
  }
}

void SnapRealm::split_at(SnapRealm *child)
{
  dout(10) << "split_at " << *child 
	   << " on " << *child->inode << dendl;

  if (inode->is_mdsdir() || !child->inode->is_dir()) {
    // it's not a dir.
    if (child->inode->containing_realm) {
      //  - no open children.
      //  - only need to move this child's inode's caps.
      child->inode->move_to_realm(child);
    } else {
      // no caps, nothing to move/split.
      dout(20) << " split no-op, no caps to move on file " << *child->inode << dendl;
      assert(!child->inode->is_any_caps());
    }
    return;
  }

  // it's a dir.

  // split open_children
  dout(10) << " open_children are " << open_children << dendl;
  for (set<SnapRealm*>::iterator p = open_children.begin();
       p != open_children.end(); ) {
    SnapRealm *realm = *p;
    if (realm != child &&
	child->inode->is_projected_ancestor_of(realm->inode)) {
      dout(20) << " child gets child realm " << *realm << " on " << *realm->inode << dendl;
      realm->parent = child;
      child->open_children.insert(realm);
      open_children.erase(p++);
    } else {
      dout(20) << "    keeping child realm " << *realm << " on " << *realm->inode << dendl;
      ++p;
    }
  }

  // split inodes_with_caps
  elist<CInode*>::iterator p = inodes_with_caps.begin(member_offset(CInode, item_caps));
  while (!p.end()) {
    CInode *in = *p;
    ++p;

    // does inode fall within the child realm?
    bool under_child = false;

    if (in == child->inode) {
      under_child = true;
    } else {
      CInode *t = in;
      while (t->get_parent_dn()) {
	t = t->get_parent_dn()->get_dir()->get_inode();
	if (t == child->inode) {
	  under_child = true;
	  break;
	}
	if (t == in)
	  break;
      }
    }
    if (under_child) {
      dout(20) << " child gets " << *in << dendl;
      in->move_to_realm(child);
    } else {
      dout(20) << "    keeping " << *in << dendl;
    }
  }

}

const bufferlist& SnapRealm::get_snap_trace()
{
  check_cache();
  return cached_snap_trace;
}

void SnapRealm::build_snap_trace(bufferlist& snapbl)
{
  SnapRealmInfo info(inode->ino(), srnode.created, srnode.seq, srnode.current_parent_since);

  if (parent) {
    info.h.parent = parent->inode->ino();
  } else {
    info.h.parent = 0;
  }

  info.my_snaps.reserve(srnode.snaps.size());
  for (map<snapid_t,SnapInfo>::reverse_iterator p = srnode.snaps.rbegin();
       p != srnode.snaps.rend();
       ++p)
    info.my_snaps.push_back(p->first);
  dout(10) << "build_snap_trace my_snaps " << info.my_snaps << dendl;

  ::encode(info, snapbl);

  if (parent)
    parent->build_snap_trace(snapbl);
}



void SnapRealm::prune_deleted_snaps()
{
  const auto& data_pools = mdcache->mds->mdsmap->get_data_pools();
  set<snapid_t> to_purge;
  snapid_t last_destroyed = 0;

  mdcache->mds->objecter->with_osdmap(
    [this, &data_pools, &to_purge, &last_destroyed](const OSDMap& osdmap) {
      auto i = this->srnode.snaps.begin();
      while (i != this->srnode.snaps.end()) {
	auto j = i++;
	bool removed = false;
	for (auto pool : data_pools) {
	  // we have to check each pool because a data pool might have been
	  // added after the snap was actually deleted, and I can't think of an
	  // efficient way for SnapClients to query this state instead.
	  const pg_pool_t *pgpool = osdmap.get_pg_pool(pool);
	  if (pgpool && pgpool->is_removed_snap(j->first)) {
	    removed = true;
	    last_destroyed = MAX(last_destroyed, pgpool->get_snap_seq());
	    break;
	  }
	}
	if (removed) {
	  to_purge.insert(j->first);
	}
      }
    });
  // TODO: SnapRealm is about to get a lot smaller; can we pull out the explicit
  // dependence on Objecter/OSDMap and write some unit tests?

  if (last_destroyed > 0) {
    for (auto snapid : to_purge) {
      srnode.snaps.erase(snapid);
    }
    assert(last_destroyed > srnode.last_destroyed);
    srnode.last_destroyed = last_destroyed;
  } else {
    assert(to_purge.empty());
  }
}

void SnapRealm::merge_snaps_from(const SnapRealm *parent)
{
  // copy parent snaps from when we became its child
  // TODO: this will let us get name updates while we're still connected, but
  // we probably only want to copy new snaps, rather than overwriting stuff.
  // Hopefully it doesn't break anything though?
  auto parent_it = parent->srnode.snaps.lower_bound(srnode.current_parent_since);
  for (; parent_it != parent->srnode.snaps.end(); ++parent_it) {
    if (!srnode.snaps.count(parent_it->first)) {
      srnode.seq = parent->srnode.seq;
      srnode.last_created = parent->srnode.last_created;
    }
    srnode.snaps[parent_it->first] = parent_it->second;
  }
}
