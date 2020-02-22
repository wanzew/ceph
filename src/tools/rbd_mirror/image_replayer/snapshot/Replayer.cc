// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "Replayer.h"
#include "common/debug.h"
#include "common/errno.h"
#include "common/Timer.h"
#include "common/WorkQueue.h"
#include "cls/rbd/cls_rbd_client.h"
#include "librbd/ImageCtx.h"
#include "librbd/ImageState.h"
#include "librbd/Utils.h"
#include "librbd/deep_copy/ImageCopyRequest.h"
#include "librbd/deep_copy/SnapshotCopyRequest.h"
#include "librbd/mirror/snapshot/CreateNonPrimaryRequest.h"
#include "librbd/mirror/snapshot/GetImageStateRequest.h"
#include "librbd/mirror/snapshot/UnlinkPeerRequest.h"
#include "tools/rbd_mirror/PoolMetaCache.h"
#include "tools/rbd_mirror/Threads.h"
#include "tools/rbd_mirror/Types.h"
#include "tools/rbd_mirror/image_replayer/CloseImageRequest.h"
#include "tools/rbd_mirror/image_replayer/ReplayerListener.h"
#include "tools/rbd_mirror/image_replayer/Utils.h"
#include "tools/rbd_mirror/image_replayer/snapshot/StateBuilder.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_rbd_mirror
#undef dout_prefix
#define dout_prefix *_dout << "rbd::mirror::image_replayer::snapshot::" \
                           << "Replayer: " << this << " " << __func__ << ": "

extern PerfCounters *g_perf_counters;

namespace rbd {
namespace mirror {
namespace image_replayer {
namespace snapshot {

using librbd::util::create_async_context_callback;
using librbd::util::create_context_callback;
using librbd::util::create_rados_callback;

template <typename I>
struct Replayer<I>::C_UpdateWatchCtx : public librbd::UpdateWatchCtx {
  Replayer<I>* replayer;

  C_UpdateWatchCtx(Replayer<I>* replayer) : replayer(replayer) {
  }

  void handle_notify() override {
     replayer->handle_remote_image_update_notify();
  }
};

template <typename I>
struct Replayer<I>::C_TrackedOp : public Context {
  Replayer *replayer;
  Context* ctx;

  C_TrackedOp(Replayer* replayer, Context* ctx)
    : replayer(replayer), ctx(ctx) {
    replayer->m_in_flight_op_tracker.start_op();
  }

  void finish(int r) override {
    ctx->complete(r);
    replayer->m_in_flight_op_tracker.finish_op();
  }
};

template <typename I>
struct Replayer<I>::ProgressContext : public librbd::ProgressContext {
  Replayer *replayer;

  ProgressContext(Replayer* replayer) : replayer(replayer) {
  }

  int update_progress(uint64_t offset, uint64_t total) override {
    replayer->handle_copy_image_progress(offset, total);
    return 0;
  }
};

template <typename I>
Replayer<I>::Replayer(
    Threads<I>* threads,
    const std::string& local_mirror_uuid,
    PoolMetaCache* pool_meta_cache,
    StateBuilder<I>* state_builder,
    ReplayerListener* replayer_listener)
  : m_threads(threads),
    m_local_mirror_uuid(local_mirror_uuid),
    m_pool_meta_cache(pool_meta_cache),
    m_state_builder(state_builder),
    m_replayer_listener(replayer_listener),
    m_lock(ceph::make_mutex(librbd::util::unique_lock_name(
      "rbd::mirror::image_replayer::snapshot::Replayer", this))) {
  dout(10) << dendl;
}

template <typename I>
Replayer<I>::~Replayer() {
  dout(10) << dendl;
  ceph_assert(m_state == STATE_COMPLETE);
  ceph_assert(m_update_watch_ctx == nullptr);
  ceph_assert(m_progress_ctx == nullptr);
}

template <typename I>
void Replayer<I>::init(Context* on_finish) {
  dout(10) << dendl;

  ceph_assert(m_state == STATE_INIT);

  RemotePoolMeta remote_pool_meta;
  int r = m_pool_meta_cache->get_remote_pool_meta(
    m_state_builder->remote_image_ctx->md_ctx.get_id(), &remote_pool_meta);
  if (r < 0 || remote_pool_meta.mirror_peer_uuid.empty()) {
    derr << "failed to retrieve mirror peer uuid from remote pool" << dendl;
    m_state = STATE_COMPLETE;
    m_threads->work_queue->queue(on_finish, r);
    return;
  }

  m_remote_mirror_peer_uuid = remote_pool_meta.mirror_peer_uuid;
  dout(10) << "remote_mirror_peer_uuid=" << m_remote_mirror_peer_uuid << dendl;

  ceph_assert(m_on_init_shutdown == nullptr);
  m_on_init_shutdown = on_finish;

  register_update_watcher();
}

template <typename I>
void Replayer<I>::shut_down(Context* on_finish) {
  dout(10) << dendl;

  std::unique_lock locker{m_lock};
  ceph_assert(m_on_init_shutdown == nullptr);
  m_on_init_shutdown = on_finish;
  m_error_code = 0;
  m_error_description = "";

  ceph_assert(m_state != STATE_INIT);
  auto state = STATE_COMPLETE;
  std::swap(m_state, state);

  if (state == STATE_REPLAYING) {
    // TODO interrupt snapshot copy and image copy state machines even if remote
    // cluster is unreachable
    dout(10) << "shut down pending on completion of snapshot replay" << dendl;
    return;
  }
  locker.unlock();

  unregister_update_watcher();
}

template <typename I>
void Replayer<I>::flush(Context* on_finish) {
  dout(10) << dendl;

  // TODO
  m_threads->work_queue->queue(on_finish, 0);
}

template <typename I>
bool Replayer<I>::get_replay_status(std::string* description,
                                    Context* on_finish) {
  dout(10) << dendl;

  *description = "NOT IMPLEMENTED";
  on_finish->complete(-EEXIST);
  return true;
}

template <typename I>
void Replayer<I>::refresh_local_image() {
  if (!m_state_builder->local_image_ctx->state->is_refresh_required()) {
    refresh_remote_image();
    return;
  }

  dout(10) << dendl;
  auto ctx = create_context_callback<
    Replayer<I>, &Replayer<I>::handle_refresh_local_image>(this);
  m_state_builder->local_image_ctx->state->refresh(ctx);
}

template <typename I>
void Replayer<I>::handle_refresh_local_image(int r) {
  dout(10) << "r=" << r << dendl;

  if (r < 0) {
    derr << "failed to refresh local image: " << cpp_strerror(r) << dendl;
    handle_replay_complete(r, "failed to refresh local image");
    return;
  }

  refresh_remote_image();
}

template <typename I>
void Replayer<I>::refresh_remote_image() {
  if (!m_state_builder->remote_image_ctx->state->is_refresh_required()) {
    scan_local_mirror_snapshots();
    return;
  }

  dout(10) << dendl;
  auto ctx = create_context_callback<
    Replayer<I>, &Replayer<I>::handle_refresh_remote_image>(this);
  m_state_builder->remote_image_ctx->state->refresh(ctx);
}

template <typename I>
void Replayer<I>::handle_refresh_remote_image(int r) {
  dout(10) << "r=" << r << dendl;

  if (r < 0) {
    derr << "failed to refresh remote image: " << cpp_strerror(r) << dendl;
    handle_replay_complete(r, "failed to refresh remote image");
    return;
  }

  scan_local_mirror_snapshots();
}

template <typename I>
void Replayer<I>::scan_local_mirror_snapshots() {
  if (is_replay_interrupted()) {
    return;
  }

  dout(10) << dendl;

  m_local_snap_id_start = 0;
  m_local_snap_id_end = CEPH_NOSNAP;
  m_local_mirror_snap_ns = {};

  m_remote_snap_id_start = 0;
  m_remote_snap_id_end = CEPH_NOSNAP;
  m_remote_mirror_snap_ns = {};

  auto local_image_ctx = m_state_builder->local_image_ctx;
  std::shared_lock image_locker{local_image_ctx->image_lock};
  for (auto snap_info_it = local_image_ctx->snap_info.begin();
       snap_info_it != local_image_ctx->snap_info.end(); ++snap_info_it) {
    const auto& snap_ns = snap_info_it->second.snap_namespace;
    auto mirror_ns = boost::get<
      cls::rbd::MirrorSnapshotNamespace>(&snap_ns);
    if (mirror_ns == nullptr) {
      continue;
    }

    dout(15) << "local mirror snapshot: id=" << snap_info_it->first << ", "
             << "mirror_ns=" << *mirror_ns << dendl;
    m_local_mirror_snap_ns = *mirror_ns;

    auto local_snap_id = snap_info_it->first;
    if (mirror_ns->is_non_primary()) {
      if (mirror_ns->complete) {
        // if remote has new snapshots, we would sync from here
        m_local_snap_id_start = local_snap_id;
        m_local_snap_id_end = CEPH_NOSNAP;
      } else {
        // start snap will be last complete mirror snapshot or initial
        // image revision
        m_local_snap_id_end = local_snap_id;
      }
    } else if (mirror_ns->is_primary()) {
      if (mirror_ns->complete) {
        m_local_snap_id_start = local_snap_id;
        m_local_snap_id_end = CEPH_NOSNAP;
      } else {
        derr << "incomplete local primary snapshot" << dendl;
        handle_replay_complete(-EINVAL, "incomplete local primary snapshot");
        return;
      }
    } else {
      derr << "unknown local mirror snapshot state" << dendl;
      handle_replay_complete(-EINVAL, "invalid local mirror snapshot state");
      return;
    }
  }
  image_locker.unlock();

  if (m_local_snap_id_start > 0 || m_local_snap_id_end != CEPH_NOSNAP) {
    if (m_local_mirror_snap_ns.is_non_primary() &&
        m_local_mirror_snap_ns.primary_mirror_uuid !=
          m_state_builder->remote_mirror_uuid) {
      // TODO support multiple peers
      derr << "local image linked to unknown peer: "
           << m_local_mirror_snap_ns.primary_mirror_uuid << dendl;
      handle_replay_complete(-EEXIST, "local image linked to unknown peer");
      return;
    } else if (m_local_mirror_snap_ns.state ==
                 cls::rbd::MIRROR_SNAPSHOT_STATE_PRIMARY) {
      dout(5) << "local image promoted" << dendl;
      handle_replay_complete(0, "force promoted");
      return;
    }

    dout(10) << "found local mirror snapshot: "
             << "local_snap_id_start=" << m_local_snap_id_start << ", "
             << "local_snap_id_end=" << m_local_snap_id_end << ", "
             << "local_snap_ns=" << m_local_mirror_snap_ns << dendl;
    if (m_local_mirror_snap_ns.complete) {
      // our remote sync should start after this completed snapshot
      m_remote_snap_id_start = m_local_mirror_snap_ns.primary_snap_id;
    }
  }

  // we don't have any mirror snapshots or only completed non-primary
  // mirror snapshots
  scan_remote_mirror_snapshots();
}

template <typename I>
void Replayer<I>::scan_remote_mirror_snapshots() {
  dout(10) << dendl;

  {
    // reset state in case new snapshot is added while we are scanning
    std::unique_lock locker{m_lock};
    m_remote_image_updated = false;
  }

  bool remote_demoted = false;
  auto remote_image_ctx = m_state_builder->remote_image_ctx;
  std::shared_lock image_locker{remote_image_ctx->image_lock};
  for (auto snap_info_it = remote_image_ctx->snap_info.begin();
       snap_info_it != remote_image_ctx->snap_info.end(); ++snap_info_it) {
    const auto& snap_ns = snap_info_it->second.snap_namespace;
    auto mirror_ns = boost::get<
      cls::rbd::MirrorSnapshotNamespace>(&snap_ns);
    if (mirror_ns == nullptr) {
      continue;
    }

    dout(15) << "remote mirror snapshot: id=" << snap_info_it->first << ", "
             << "mirror_ns=" << *mirror_ns << dendl;
    if (!mirror_ns->is_primary() && !mirror_ns->is_non_primary()) {
      derr << "unknown remote mirror snapshot state" << dendl;
      handle_replay_complete(-EINVAL, "invalid remote mirror snapshot state");
      return;
    } else {
      remote_demoted = (mirror_ns->is_primary() && mirror_ns->is_demoted());
    }

    auto remote_snap_id = snap_info_it->first;
    if (m_local_snap_id_start > 0 || m_local_snap_id_end != CEPH_NOSNAP) {
      // we have a local mirror snapshot
      if (m_local_mirror_snap_ns.is_non_primary()) {
        // previously validated that it was linked to remote
        ceph_assert(m_local_mirror_snap_ns.primary_mirror_uuid ==
                      m_state_builder->remote_mirror_uuid);

        if (m_local_mirror_snap_ns.complete &&
            m_local_mirror_snap_ns.primary_snap_id >= remote_snap_id) {
          // skip past completed remote snapshot
          m_remote_snap_id_start = remote_snap_id;
          dout(15) << "skipping synced remote snapshot " << remote_snap_id
                   << dendl;
          continue;
        } else if (!m_local_mirror_snap_ns.complete &&
                   m_local_mirror_snap_ns.primary_snap_id > remote_snap_id) {
          // skip until we get to the in-progress remote snapshot
          dout(15) << "skipping synced remote snapshot " << remote_snap_id
                   << " while search for in-progress sync" << dendl;
          m_remote_snap_id_start = remote_snap_id;
          continue;
        }
      } else if (m_local_mirror_snap_ns.state ==
                   cls::rbd::MIRROR_SNAPSHOT_STATE_PRIMARY_DEMOTED) {
        // find the matching demotion snapshot in remote image
        ceph_assert(m_local_snap_id_start > 0);
        if (mirror_ns->state ==
              cls::rbd::MIRROR_SNAPSHOT_STATE_NON_PRIMARY_DEMOTED &&
            mirror_ns->primary_mirror_uuid == m_local_mirror_uuid &&
            mirror_ns->primary_snap_id == m_local_snap_id_start) {
          dout(10) << "located matching demotion snapshot: "
                   << "remote_snap_id=" << remote_snap_id << ", "
                   << "local_snap_id=" << m_local_snap_id_start << dendl;
          m_remote_snap_id_start = remote_snap_id;
          continue;
        } else if (m_remote_snap_id_start == 0) {
          // still looking for our matching demotion snapshot
          dout(15) << "skipping remote snapshot " << remote_snap_id << " "
                   << "while searching for demotion" << dendl;
          continue;
        }
      } else {
        // should not have been able to reach this
        ceph_assert(false);
      }
    }

    // find first snapshot where were are listed as a peer
    if (!mirror_ns->is_primary()) {
      dout(15) << "skipping non-primary remote snapshot" << dendl;
      continue;
    } else if (mirror_ns->mirror_peer_uuids.count(m_remote_mirror_peer_uuid) ==
                 0) {
      dout(15) << "skipping remote snapshot due to missing mirror peer"
               << dendl;
      continue;
    }

    m_remote_snap_id_end = remote_snap_id;
    m_remote_mirror_snap_ns = *mirror_ns;
    break;
  }
  image_locker.unlock();

  if (m_remote_snap_id_end != CEPH_NOSNAP) {
    dout(10) << "found remote mirror snapshot: "
             << "remote_snap_id_start=" << m_remote_snap_id_start << ", "
             << "remote_snap_id_end=" << m_remote_snap_id_end << ", "
             << "remote_snap_ns=" << m_remote_mirror_snap_ns << dendl;
    if (m_remote_mirror_snap_ns.complete) {
      if (m_local_snap_id_end != CEPH_NOSNAP &&
          !m_local_mirror_snap_ns.complete) {
        // attempt to resume image-sync
        dout(10) << "local image contains in-progress mirror snapshot"
                 << dendl;
        copy_image();
      } else {
        copy_snapshots();
      }
      return;
    } else {
      // might have raced with the creation of a remote mirror snapshot
      // so we will need to refresh and rescan once it completes
      dout(15) << "remote mirror snapshot not complete" << dendl;
    }
  }

  std::unique_lock locker{m_lock};
  if (m_remote_image_updated) {
    // received update notification while scanning image, restart ...
    m_remote_image_updated = false;
    locker.unlock();

    dout(10) << "restarting snapshot scan due to remote update notification"
             << dendl;
    refresh_local_image();
    return;
  }

  if (is_replay_interrupted(&locker)) {
    return;
  } else if (remote_demoted) {
    locker.unlock();

    dout(10) << "remote image demoted" << dendl;
    handle_replay_complete(0, "remote image demoted");
    return;
  }

  dout(10) << "all remote snapshots synced: idling waiting for new snapshot"
           << dendl;
  ceph_assert(m_state == STATE_REPLAYING);
  m_state = STATE_IDLE;

  notify_status_updated();
}

template <typename I>
void Replayer<I>::copy_snapshots() {
  dout(10) << dendl;

  ceph_assert(m_remote_snap_id_start != CEPH_NOSNAP);
  ceph_assert(m_remote_snap_id_end > 0 &&
              m_remote_snap_id_end != CEPH_NOSNAP);
  ceph_assert(m_local_snap_id_start != CEPH_NOSNAP);

  m_local_mirror_snap_ns = {};
  auto ctx = create_context_callback<
    Replayer<I>, &Replayer<I>::handle_copy_snapshots>(this);
  auto req = librbd::deep_copy::SnapshotCopyRequest<I>::create(
    m_state_builder->remote_image_ctx, m_state_builder->local_image_ctx,
    m_remote_snap_id_start, m_remote_snap_id_end, m_local_snap_id_start,
    false, m_threads->work_queue, &m_local_mirror_snap_ns.snap_seqs,
    ctx);
  req->send();
}

template <typename I>
void Replayer<I>::handle_copy_snapshots(int r) {
  dout(10) << "r=" << r << dendl;

  if (r < 0) {
    derr << "failed to copy snapshots from remote to local image: "
         << cpp_strerror(r) << dendl;
    handle_replay_complete(
      r, "failed to copy snapshots from remote to local image");
    return;
  }

  dout(10) << "remote_snap_id_start=" << m_remote_snap_id_start << ", "
           << "remote_snap_id_end=" << m_remote_snap_id_end << ", "
           << "local_snap_id_start=" << m_local_snap_id_start << ", "
           << "snap_seqs=" << m_local_mirror_snap_ns.snap_seqs << dendl;
  get_image_state();
}

template <typename I>
void Replayer<I>::get_image_state() {
  dout(10) << dendl;

  auto ctx = create_context_callback<
    Replayer<I>, &Replayer<I>::handle_get_image_state>(this);
  auto req = librbd::mirror::snapshot::GetImageStateRequest<I>::create(
    m_state_builder->remote_image_ctx, m_remote_snap_id_end,
    &m_image_state, ctx);
  req->send();
}

template <typename I>
void Replayer<I>::handle_get_image_state(int r) {
  dout(10) << "r=" << r << dendl;

  if (r < 0) {
    derr << "failed to retrieve remote snapshot image state: "
         << cpp_strerror(r) << dendl;
    handle_replay_complete(r, "failed to retrieve remote snapshot image state");
    return;
  }

  create_non_primary_snapshot();
}

template <typename I>
void Replayer<I>::create_non_primary_snapshot() {
  dout(10) << dendl;

  auto ctx = create_context_callback<
    Replayer<I>, &Replayer<I>::handle_create_non_primary_snapshot>(this);
  auto req = librbd::mirror::snapshot::CreateNonPrimaryRequest<I>::create(
    m_state_builder->local_image_ctx, m_remote_mirror_snap_ns.is_demoted(),
    m_state_builder->remote_mirror_uuid, m_remote_snap_id_end,
    m_local_mirror_snap_ns.snap_seqs, m_image_state, &m_local_snap_id_end, ctx);
  req->send();
}

template <typename I>
void Replayer<I>::handle_create_non_primary_snapshot(int r) {
  dout(10) << "r=" << r << dendl;

  if (r < 0) {
    derr << "failed to create local mirror snapshot: " << cpp_strerror(r)
         << dendl;
    handle_replay_complete(r, "failed to create local mirror snapshot");
    return;
  }

  copy_image();
}

template <typename I>
void Replayer<I>::copy_image() {
  dout(10) << dendl;

  m_progress_ctx = new ProgressContext(this);
  auto ctx = create_context_callback<
    Replayer<I>, &Replayer<I>::handle_copy_image>(this);
  auto req = librbd::deep_copy::ImageCopyRequest<I>::create(
    m_state_builder->remote_image_ctx,  m_state_builder->local_image_ctx,
    m_remote_snap_id_start, m_remote_snap_id_end, m_local_snap_id_start, false,
    (m_local_mirror_snap_ns.last_copied_object_number > 0 ?
      librbd::deep_copy::ObjectNumber{
        m_local_mirror_snap_ns.last_copied_object_number} :
      librbd::deep_copy::ObjectNumber{}),
    m_local_mirror_snap_ns.snap_seqs, m_progress_ctx, ctx);
  req->send();
}

template <typename I>
void Replayer<I>::handle_copy_image(int r) {
  dout(10) << "r=" << r << dendl;

  delete m_progress_ctx;
  m_progress_ctx = nullptr;

  if (r < 0) {
    derr << "failed to copy remote image to local image: " << cpp_strerror(r)
         << dendl;
    handle_replay_complete(r, "failed to copy remote image");
    return;
  }

  update_non_primary_snapshot(true);
}

template <typename I>
void Replayer<I>::handle_copy_image_progress(uint64_t offset, uint64_t total) {
  dout(10) << "offset=" << offset << ", total=" << total << dendl;

  // TODO
}

template <typename I>
void Replayer<I>::update_non_primary_snapshot(bool complete) {
  dout(10) << dendl;

  if (complete) {
    m_local_mirror_snap_ns.complete = true;
  }

  librados::ObjectWriteOperation op;
  librbd::cls_client::mirror_image_snapshot_set_copy_progress(
    &op, m_local_snap_id_end, m_local_mirror_snap_ns.complete,
    m_local_mirror_snap_ns.last_copied_object_number);

  auto ctx = new LambdaContext([this, complete](int r) {
      handle_update_non_primary_snapshot(complete, r);
    });
  auto aio_comp = create_rados_callback(ctx);
  int r = m_state_builder->local_image_ctx->md_ctx.aio_operate(
    m_state_builder->local_image_ctx->header_oid, aio_comp, &op);
  ceph_assert(r == 0);
  aio_comp->release();
}

template <typename I>
void Replayer<I>::handle_update_non_primary_snapshot(bool complete, int r) {
  dout(10) << "r=" << r << dendl;

  if (r < 0) {
    derr << "failed to update local snapshot progress: " << cpp_strerror(r)
         << dendl;
    handle_replay_complete(r, "failed to update local snapshot progress");
    return;
  }

  notify_image_update();
}

template <typename I>
void Replayer<I>::notify_image_update() {
  dout(10) << dendl;

  auto ctx = create_context_callback<
    Replayer<I>, &Replayer<I>::handle_notify_image_update>(this);
  m_state_builder->local_image_ctx->notify_update(ctx);
}

template <typename I>
void Replayer<I>::handle_notify_image_update(int r) {
  dout(10) << "r=" << r << dendl;

  if (r < 0) {
    derr << "failed to notify local image update: " << cpp_strerror(r) << dendl;
  }

  if (is_replay_interrupted()) {
    return;
  }

  unlink_peer();
}

template <typename I>
void Replayer<I>::unlink_peer() {
  if (m_remote_snap_id_start == 0) {
    {
      std::unique_lock locker{m_lock};
      notify_status_updated();
    }

    refresh_local_image();
    return;
  }

  // local snapshot fully synced -- we no longer depend on the sync
  // start snapshot in the remote image
  dout(10) << "remote_snap_id=" << m_remote_snap_id_start << dendl;

  auto ctx = create_context_callback<
    Replayer<I>, &Replayer<I>::handle_unlink_peer>(this);
  auto req = librbd::mirror::snapshot::UnlinkPeerRequest<I>::create(
    m_state_builder->remote_image_ctx, m_remote_snap_id_start,
    m_remote_mirror_peer_uuid, ctx);
  req->send();
}

template <typename I>
void Replayer<I>::handle_unlink_peer(int r) {
  dout(10) << "r=" << r << dendl;

  if (r < 0 && r != -ENOENT) {
    derr << "failed to unlink local peer from remote image: " << cpp_strerror(r)
         << dendl;
    handle_replay_complete(r, "failed to unlink local peer from remote image");
    return;
  }

  {
    std::unique_lock locker{m_lock};
    notify_status_updated();
  }

  refresh_local_image();
}

template <typename I>
void Replayer<I>::register_update_watcher() {
  dout(10) << dendl;

  m_update_watch_ctx = new C_UpdateWatchCtx(this);
  int r = m_state_builder->remote_image_ctx->state->register_update_watcher(
    m_update_watch_ctx, &m_update_watcher_handle);
  auto ctx = create_context_callback<
    Replayer<I>, &Replayer<I>::handle_register_update_watcher>(this);
  m_threads->work_queue->queue(ctx, r);
}

template <typename I>
void Replayer<I>::handle_register_update_watcher(int r) {
  dout(10) << "r=" << r << dendl;

  if (r < 0) {
    derr << "failed to register update watcher: " << cpp_strerror(r) << dendl;
    handle_replay_complete(r, "failed to register remote image update watcher");
    m_state = STATE_COMPLETE;

    delete m_update_watch_ctx;
    m_update_watch_ctx = nullptr;
  } else {
    m_state = STATE_REPLAYING;
  }

  Context* on_init = nullptr;
  std::swap(on_init, m_on_init_shutdown);
  on_init->complete(r);

  // delay initial snapshot scan until after we have alerted
  // image replayer that we have initialized in case an error
  // occurs
  if (r >= 0) {
    {
      std::unique_lock locker{m_lock};
      notify_status_updated();
    }

    refresh_local_image();
  }
}

template <typename I>
void Replayer<I>::unregister_update_watcher() {
  dout(10) << dendl;

  auto ctx = create_context_callback<
    Replayer<I>,
    &Replayer<I>::handle_unregister_update_watcher>(this);
  m_state_builder->remote_image_ctx->state->unregister_update_watcher(
    m_update_watcher_handle, ctx);
}

template <typename I>
void Replayer<I>::handle_unregister_update_watcher(int r) {
  dout(10) << "r=" << r << dendl;

  if (r < 0) {
    derr << "failed to unregister update watcher: " << cpp_strerror(r) << dendl;
    handle_replay_complete(
      r, "failed to unregister remote image update watcher");
  }

  delete m_update_watch_ctx;
  m_update_watch_ctx = nullptr;

  wait_for_in_flight_ops();
}

template <typename I>
void Replayer<I>::wait_for_in_flight_ops() {
  dout(10) << dendl;

  auto ctx = create_async_context_callback(
    m_threads->work_queue, create_context_callback<
      Replayer<I>, &Replayer<I>::handle_wait_for_in_flight_ops>(this));
  m_in_flight_op_tracker.wait_for_ops(ctx);
}

template <typename I>
void Replayer<I>::handle_wait_for_in_flight_ops(int r) {
  dout(10) << "r=" << r << dendl;

  Context* on_shutdown = nullptr;
  {
    std::unique_lock locker{m_lock};
    ceph_assert(m_on_init_shutdown != nullptr);
    std::swap(on_shutdown, m_on_init_shutdown);
  }
  on_shutdown->complete(m_error_code);
}

template <typename I>
void Replayer<I>::handle_remote_image_update_notify() {
  dout(10) << dendl;

  std::unique_lock locker{m_lock};
  if (m_state == STATE_REPLAYING) {
    dout(15) << "flagging snapshot rescan required" << dendl;
    m_remote_image_updated = true;
  } else if (m_state == STATE_IDLE) {
    m_state = STATE_REPLAYING;
    locker.unlock();

    dout(15) << "restarting idle replayer" << dendl;
    refresh_local_image();
  }
}

template <typename I>
void Replayer<I>::handle_replay_complete(int r,
                                         const std::string& description) {
  std::unique_lock locker{m_lock};
  if (m_error_code == 0) {
    m_error_code = r;
    m_error_description = description;
  }

  if (m_state != STATE_REPLAYING && m_state != STATE_IDLE) {
    return;
  }

  m_state = STATE_COMPLETE;
  notify_status_updated();
}

template <typename I>
void Replayer<I>::notify_status_updated() {
  ceph_assert(ceph_mutex_is_locked_by_me(m_lock));

  dout(10) << dendl;
  auto ctx = new C_TrackedOp(this, new LambdaContext(
    [this](int) {
      m_replayer_listener->handle_notification();
    }));
  m_threads->work_queue->queue(ctx, 0);
}

template <typename I>
bool Replayer<I>::is_replay_interrupted() {
  std::unique_lock locker{m_lock};
  return is_replay_interrupted(&locker);
}

template <typename I>
bool Replayer<I>::is_replay_interrupted(std::unique_lock<ceph::mutex>* locker) {
  if (m_state == STATE_COMPLETE) {
    locker->unlock();

    dout(10) << "resuming pending shut down" << dendl;
    unregister_update_watcher();
    return true;
  }
  return false;
}

} // namespace snapshot
} // namespace image_replayer
} // namespace mirror
} // namespace rbd

template class rbd::mirror::image_replayer::snapshot::Replayer<librbd::ImageCtx>;
