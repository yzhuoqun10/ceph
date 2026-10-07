// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#ifndef CEPH_LIBRBD_MIRROR_SNAPSHOT_RECORD_LINEAGE_REQUEST_H
#define CEPH_LIBRBD_MIRROR_SNAPSHOT_RECORD_LINEAGE_REQUEST_H

#include "include/buffer.h"
#include "librbd/mirror/snapshot/ImageMeta.h"

#include <string>

struct Context;

namespace librbd {

struct ImageCtx;

namespace mirror {
namespace snapshot {

template <typename ImageCtxT = librbd::ImageCtx>
class RecordLineageRequest {
public:
  static RecordLineageRequest *create(ImageCtxT *image_ctx,
                                      const LineageEntry& entry,
                                      Context *on_finish) {
    return new RecordLineageRequest(image_ctx, entry, on_finish);
  }

  RecordLineageRequest(ImageCtxT *image_ctx, const LineageEntry& entry,
                       Context *on_finish)
    : m_image_ctx(image_ctx), m_entry(entry), m_on_finish(on_finish) {
  }

  void send();

private:
  /**
   * @verbatim
   *
   * <start>
   *    |
   *    v
   * GET_MIRROR_UUID
   *    |
   *    v
   * LOAD_IMAGE_META (ENOENT ok)
   *    |
   *    v
   * SAVE_IMAGE_META
   *    |
   *    v
   * <finish>
   *
   * @endverbatim
   */

  ImageCtxT *m_image_ctx;
  LineageEntry m_entry;
  Context *m_on_finish;

  std::string m_mirror_uuid;
  ImageMeta<ImageCtxT>* m_image_meta = nullptr;

  void get_mirror_uuid();
  void handle_get_mirror_uuid(int r);

  void load_image_meta();
  void handle_load_image_meta(int r);

  void save_image_meta();
  void handle_save_image_meta(int r);

  void finish(int r);
};

} // namespace snapshot
} // namespace mirror
} // namespace librbd

extern template class librbd::mirror::snapshot::RecordLineageRequest<librbd::ImageCtx>;

#endif // CEPH_LIBRBD_MIRROR_SNAPSHOT_RECORD_LINEAGE_REQUEST_H
