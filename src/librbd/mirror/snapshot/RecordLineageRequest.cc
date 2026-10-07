// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#include "librbd/mirror/snapshot/RecordLineageRequest.h"
#include "common/dout.h"
#include "common/errno.h"
#include "librbd/ImageCtx.h"
#include "librbd/Utils.h"
#include "librbd/mirror/GetUuidRequest.h"

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::mirror::snapshot::RecordLineageRequest: " \
                           << this << " " << __func__ << ": "

namespace librbd {
namespace mirror {
namespace snapshot {

using librbd::util::create_context_callback;

template <typename I>
void RecordLineageRequest<I>::send() {
  get_mirror_uuid();
}

template <typename I>
void RecordLineageRequest<I>::get_mirror_uuid() {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 15) << dendl;

  auto ctx = create_context_callback<
    RecordLineageRequest<I>,
    &RecordLineageRequest<I>::handle_get_mirror_uuid>(this);
  auto req = GetUuidRequest<I>::create(m_image_ctx->md_ctx, &m_mirror_uuid,
                                       ctx);
  req->send();
}

template <typename I>
void RecordLineageRequest<I>::handle_get_mirror_uuid(int r) {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 15) << "r=" << r << dendl;

  if (r < 0) {
    lderr(cct) << "failed to retrieve mirror uuid: " << cpp_strerror(r)
               << dendl;
    finish(r);
    return;
  }

  load_image_meta();
}

template <typename I>
void RecordLineageRequest<I>::load_image_meta() {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 15) << "mirror_uuid=" << m_mirror_uuid << dendl;

  ceph_assert(m_image_meta == nullptr);
  m_image_meta = ImageMeta<I>::create(m_image_ctx, m_mirror_uuid);

  auto ctx = create_context_callback<
    RecordLineageRequest<I>,
    &RecordLineageRequest<I>::handle_load_image_meta>(this);
  m_image_meta->load(ctx);
}

template <typename I>
void RecordLineageRequest<I>::handle_load_image_meta(int r) {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 15) << "r=" << r << dendl;

  if (r < 0 && r != -ENOENT) {
    lderr(cct) << "failed to load image-meta: " << cpp_strerror(r) << dendl;
    finish(r);
    return;
  }

  save_image_meta();
}

template <typename I>
void RecordLineageRequest<I>::save_image_meta() {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 15) << "entry=" << m_entry << dendl;

  m_image_meta->add_lineage(m_entry);

  auto ctx = create_context_callback<
    RecordLineageRequest<I>,
    &RecordLineageRequest<I>::handle_save_image_meta>(this);
  m_image_meta->save(ctx);
}

template <typename I>
void RecordLineageRequest<I>::handle_save_image_meta(int r) {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 15) << "r=" << r << dendl;

  if (r < 0) {
    lderr(cct) << "failed to save image-meta: " << cpp_strerror(r) << dendl;
  }
  finish(r);
}

template <typename I>
void RecordLineageRequest<I>::finish(int r) {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 15) << "r=" << r << dendl;

  delete m_image_meta;
  m_on_finish->complete(r);
  delete this;
}

} // namespace snapshot
} // namespace mirror
} // namespace librbd

template class librbd::mirror::snapshot::RecordLineageRequest<librbd::ImageCtx>;
