// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#ifndef CEPH_LIBRBD_MIRROR_SNAPSHOT_IMAGE_META_H
#define CEPH_LIBRBD_MIRROR_SNAPSHOT_IMAGE_META_H

#include "include/rados/librados.hpp"
#include "include/rados.h"
#include "include/types.h"
#include <iosfwd>
#include <map>
#include <string>
#include <vector>

struct Context;

namespace librbd {

struct ImageCtx;

namespace mirror {
namespace snapshot {

// Records that this image was promoted to primary while it was a non-primary
// replica of another site. Lets a third site that was also replicating from
// that previous primary verify the new primary's lineage and re-link to it
// without a full resync. Persisted in image-meta (not in a snapshot) because
// mirror snapshots are pruned/rotated aggressively.
struct LineageEntry {
  std::string from_mirror_uuid;          // mirror uuid of the previous primary pool
  uint64_t from_snap_id = CEPH_NOSNAP;   // primary_snap_id of the anchor on the previous primary
  uint64_t local_snap_id = CEPH_NOSNAP;  // anchor (non-primary) snapshot id on this image
  uint64_t promote_snap_id = CEPH_NOSNAP; // the PRIMARY snapshot created by promote
  bool forced = false;
  std::map<uint64_t, uint64_t> snap_seqs; // previous primary snap id -> local snap id
  uint64_t timestamp = 0;                 // unix seconds

  bool operator==(const LineageEntry& rhs) const {
    return from_mirror_uuid == rhs.from_mirror_uuid &&
           from_snap_id == rhs.from_snap_id &&
           local_snap_id == rhs.local_snap_id &&
           promote_snap_id == rhs.promote_snap_id &&
           forced == rhs.forced &&
           snap_seqs == rhs.snap_seqs &&
           timestamp == rhs.timestamp;
  }
};

std::ostream& operator<<(std::ostream& os, const LineageEntry& entry);

template <typename ImageCtxT>
class ImageMeta {
public:
  static constexpr size_t MAX_LINEAGE_ENTRIES = 8;

  static ImageMeta* create(ImageCtxT* image_ctx,
                           const std::string& mirror_uuid) {
    return new ImageMeta(image_ctx, mirror_uuid);
  }

  ImageMeta(ImageCtxT* image_ctx, const std::string& mirror_uuid);

  void load(Context* on_finish);
  void save(Context* on_finish);

  // append a promote record, keeping only the most recent entries
  void add_lineage(const LineageEntry& entry);

  bool resync_requested = false;
  std::vector<LineageEntry> lineage;

private:
  /**
   * @verbatim
   *
   * <start>
   *   |
   *   v
   * METADATA_GET
   *   |
   *   v
   * <idle>
   *   |
   *   v
   * METADATA_SET
   *   |
   *   v
   * NOTIFY_UPDATE
   *   |
   *   v
   * <finish>
   *
   * @endverbatim
   */

  ImageCtxT* m_image_ctx;
  std::string m_mirror_uuid;

  bufferlist m_out_bl;

  void handle_load(Context* on_finish, int r);

  void handle_save(Context* on_finish, int r);

  void notify_update(Context* on_finish);
  void handle_notify_update(Context* on_finish, int r);

};

} // namespace snapshot
} // namespace mirror
} // namespace librbd

extern template class librbd::mirror::snapshot::ImageMeta<librbd::ImageCtx>;

#endif // CEPH_LIBRBD_MIRROR_SNAPSHOT_IMAGE_META_H
