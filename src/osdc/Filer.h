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


#ifndef __FILER_H
#define __FILER_H

/*** Filer
 *
 * stripe file ranges onto objects.
 * build list<ObjectExtent> for the objecter or objectcacher.
 *
 * also, provide convenience methods that call objecter for you.
 *
 * "files" are identified by ino. 
 */

#include "include/types.h"

#include "osd/OSDMap.h"
#include "Objecter.h"

class Context;
class Messenger;
class OSDMap;


/**** Filer interface ***/

class Filer {
  Objecter   *objecter;
  
  // probes
  struct Probe {
    inodeno_t ino;
    ceph_file_layout layout;
    snapid_t snapid;
    __u64 from;        // for !fwd, this is start of extent we are probing, thus possibly < our endpoint.
    __u64 *end;
    int flags;

    bool fwd;

    Context *onfinish;
    
    list<ObjectExtent> probing;
    __u64 probing_len;
    
    map<object_t, __u64> known;
    map<object_t, tid_t> ops;

    Probe(inodeno_t i, ceph_file_layout &l, snapid_t sn,
	  __u64 f, __u64 *e, int fl, bool fw, Context *c) : 
      ino(i), layout(l), snapid(sn),
      from(f), end(e), flags(fl), fwd(fw), onfinish(c), probing_len(0) {}
  };
  
  class C_Probe;

  void _probe(Probe *p);
  void _probed(Probe *p, object_t oid, __u64 size);

 public:
  Filer(Objecter *o) : objecter(o) {}
  ~Filer() {}

  bool is_active() {
    return objecter->is_active(); // || (oc && oc->is_active());
  }

  /*** async file interface ***/
  Objecter::OSDRead *prepare_read(inodeno_t ino,
				  ceph_file_layout *layout,
				  snapid_t snapid,
				  __u64 offset, 
				  size_t len, 
				  bufferlist *bl, 
				  int flags) {
    Objecter::OSDRead *rd = objecter->prepare_read(bl, flags);
    file_to_extents(ino, layout, snapid, offset, len, rd->extents);
    return rd;
  }
  int read(inodeno_t ino,
	   ceph_file_layout *layout,
	   snapid_t snapid,
           __u64 offset, 
           size_t len, 
           bufferlist *bl,   // ptr to data
	   int flags,
           Context *onfinish) {
    Objecter::OSDRead *rd = prepare_read(ino, layout, snapid, offset, len, bl, flags);
    return objecter->readx(rd, onfinish) > 0 ? 0:-1;
  }

  int write(inodeno_t ino,
	    ceph_file_layout *layout,
	    const SnapContext& snapc,
	    __u64 offset, 
            size_t len, 
            bufferlist& bl,
            int flags, 
            Context *onack,
            Context *oncommit) {
    Objecter::OSDWrite *wr = objecter->prepare_write(snapc, bl, flags);
    file_to_extents(ino, layout, CEPH_NOSNAP, offset, len, wr->extents);
    return objecter->modifyx(wr, onack, oncommit) > 0 ? 0:-1;
  }

  int zero(inodeno_t ino,
	   ceph_file_layout *layout,
	   const SnapContext& snapc,
	   __u64 offset,
           size_t len,
	   int flags,
           Context *onack,
           Context *oncommit) {
    Objecter::OSDModify *z = objecter->prepare_modify(snapc, CEPH_OSD_OP_ZERO, flags);
    file_to_extents(ino, layout, CEPH_NOSNAP, offset, len, z->extents);
    return objecter->modifyx(z, onack, oncommit) > 0 ? 0:-1;
  }

  int remove(inodeno_t ino,
	     ceph_file_layout *layout,
	     const SnapContext& snapc,
	     __u64 offset,
	     size_t len,
	     int flags,
	     Context *onack,
	     Context *oncommit) {
    Objecter::OSDModify *z = objecter->prepare_modify(snapc, CEPH_OSD_OP_DELETE, flags);
    file_to_extents(ino, layout, CEPH_NOSNAP, offset, len, z->extents);
    return objecter->modifyx(z, onack, oncommit) > 0 ? 0:-1;
  }

  /*
   * probe 
   *  specify direction,
   *  and whether we stop when we find data, or hole.
   */
  int probe(inodeno_t ino,
	    ceph_file_layout *layout,
	    snapid_t snapid,
	    __u64 start_from,
	    __u64 *end,
	    bool fwd,
	    int flags,
	    Context *onfinish);


  /***** mapping *****/

  /*
   * map (ino, layout, offset, len) to a (list of) OSDExtents (byte
   * ranges in objects on (primary) osds)
   */
  void file_to_extents(inodeno_t ino, ceph_file_layout *layout, snapid_t snap,
		       __u64 offset,
		       size_t len,
		       list<ObjectExtent>& extents);
  
};



#endif
