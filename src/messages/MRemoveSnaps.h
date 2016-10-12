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

#ifndef CEPH_MREMOVESNAPS_H
#define CEPH_MREMOVESNAPS_H

#include "messages/PaxosServiceMessage.h"

struct MRemoveSnaps : public PaxosServiceMessage {
  static const int HEAD_VERSION = 2;
  static const int COMPAT_VERSION = 2;
  map<int, vector<snapid_t> > snaps;
  map<int, snapid_t> snap_seqs;
  
  // TODO: What the heck is this 0 in PaxosServiceMessage? It's a "version"; is
  // it just an outdated scheme we should zap?
  MRemoveSnaps() : 
    PaxosServiceMessage(MSG_REMOVE_SNAPS, 0, HEAD_VERSION, COMPAT_VERSION)  { }
  MRemoveSnaps(map<int, vector<snapid_t> >& s, map<int, snapid_t> seqs) : 
    PaxosServiceMessage(MSG_REMOVE_SNAPS, 0, HEAD_VERSION, COMPAT_VERSION) {
    snaps.swap(s);
    snap_seqs.swap(seqs);
  }
private:
  ~MRemoveSnaps() {}

public:
  const char *get_type_name() const { return "remove_snaps"; }
  void print(ostream& out) const {
    out << "remove_snaps(" << snaps << " v" << version << ")";
  }

  void encode_payload(uint64_t features) {
    paxos_encode();
    ::encode(snaps, payload);
    ::encode(snap_seqs, payload);
    // TODO: I should probably just kill this, right? It's polite to
    // older monitors but otherwise just noise and those are usually
    // upgraded first
    bool can_skip_seqs = true;
    for (auto i : snap_seqs) {
      if (0 != i.second) {
	can_skip_seqs = false;
	break;
      }
    }
    if (can_skip_seqs) {
      header.compat_version = 0;
    }
  }
  void decode_payload() {
    bufferlist::iterator p = payload.begin();
    paxos_decode(p);
    ::decode(snaps, p);
    if (header.version >= 2) {
      ::decode(snap_seqs, p);
    }
  }

};

#endif
