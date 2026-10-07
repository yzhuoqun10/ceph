// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#include "test/librbd/test_mock_fixture.h"
#include "test/librbd/test_support.h"
#include "test/librbd/mock/MockImageCtx.h"
#include "test/librados_test_stub/MockTestMemIoCtxImpl.h"
#include "librbd/ImageState.h"
#include "librbd/mirror/snapshot/ImageMeta.h"
#include "librbd/mirror/snapshot/Utils.h"

namespace librbd {
namespace {

struct MockTestImageCtx : public MockImageCtx {
  MockTestImageCtx(librbd::ImageCtx& image_ctx) : MockImageCtx(image_ctx) {
  }
};

} // anonymous namespace
} // namespace librbd

#include "librbd/mirror/snapshot/ImageMeta.cc"

namespace librbd {
namespace mirror {
namespace snapshot {

using ::testing::_;
using ::testing::DoAll;
using ::testing::InSequence;
using ::testing::Invoke;
using ::testing::Return;
using ::testing::StrEq;
using ::testing::WithArg;

class TestMockMirrorSnapshotImageMeta : public TestMockFixture {
public:
  typedef ImageMeta<MockTestImageCtx> MockImageMeta;

  void expect_metadata_get(MockTestImageCtx& mock_image_ctx,
                           const std::string& mirror_uuid,
                           const std::string& value, int r) {
    bufferlist in_bl;
    ceph::encode(util::get_image_meta_key(mirror_uuid), in_bl);

    bufferlist out_bl;
    ceph::encode(value, out_bl);

    EXPECT_CALL(get_mock_io_ctx(mock_image_ctx.md_ctx),
                exec_internal(mock_image_ctx.header_oid, _, StrEq("rbd"),
                     StrEq("metadata_get"), ContentsEqual(in_bl), _, _, _))
      .WillOnce(DoAll(WithArg<5>(CopyInBufferlist(out_bl)),
                      Return(r)));
  }

  void expect_metadata_set(MockTestImageCtx& mock_image_ctx,
                           const std::string& mirror_uuid,
                           const std::string& value, int r) {
    bufferlist value_bl;
    value_bl.append(value);

    bufferlist in_bl;
    ceph::encode(
      std::map<std::string, bufferlist>{
        {util::get_image_meta_key(mirror_uuid), value_bl}},
      in_bl);

    EXPECT_CALL(get_mock_io_ctx(mock_image_ctx.md_ctx),
                exec_internal(mock_image_ctx.header_oid, _, StrEq("rbd"),
                     StrEq("metadata_set"), ContentsEqual(in_bl), _, _, _))
      .WillOnce(Return(r));
  }
};

TEST_F(TestMockMirrorSnapshotImageMeta, Load) {
  librbd::ImageCtx* image_ctx;
  ASSERT_EQ(0, open_image(m_image_name, &image_ctx));
  MockTestImageCtx mock_image_ctx(*image_ctx);

  InSequence seq;
  expect_metadata_get(mock_image_ctx, "mirror uuid",
                      "{\"resync_requested\": true}", 0);

  MockImageMeta mock_image_meta(&mock_image_ctx, "mirror uuid");
  C_SaferCond ctx;
  mock_image_meta.load(&ctx);
  ASSERT_EQ(0, ctx.wait());
}

TEST_F(TestMockMirrorSnapshotImageMeta, LoadError) {
  librbd::ImageCtx* image_ctx;
  ASSERT_EQ(0, open_image(m_image_name, &image_ctx));
  MockTestImageCtx mock_image_ctx(*image_ctx);

  InSequence seq;
  expect_metadata_get(mock_image_ctx, "mirror uuid",
                      "{\"resync_requested\": true}", -EINVAL);

  MockImageMeta mock_image_meta(&mock_image_ctx, "mirror uuid");
  C_SaferCond ctx;
  mock_image_meta.load(&ctx);
  ASSERT_EQ(-EINVAL, ctx.wait());
}

TEST_F(TestMockMirrorSnapshotImageMeta, LoadCorrupt) {
  librbd::ImageCtx* image_ctx;
  ASSERT_EQ(0, open_image(m_image_name, &image_ctx));
  MockTestImageCtx mock_image_ctx(*image_ctx);

  InSequence seq;
  expect_metadata_get(mock_image_ctx, "mirror uuid",
                      "\"resync_requested\": true}", 0);

  MockImageMeta mock_image_meta(&mock_image_ctx, "mirror uuid");
  C_SaferCond ctx;
  mock_image_meta.load(&ctx);
  ASSERT_EQ(-EBADMSG, ctx.wait());
}

TEST_F(TestMockMirrorSnapshotImageMeta, LoadLegacyWithoutLineage) {
  librbd::ImageCtx* image_ctx;
  ASSERT_EQ(0, open_image(m_image_name, &image_ctx));
  MockTestImageCtx mock_image_ctx(*image_ctx);

  InSequence seq;
  expect_metadata_get(mock_image_ctx, "mirror uuid",
                      "{\"resync_requested\": false}", 0);

  MockImageMeta mock_image_meta(&mock_image_ctx, "mirror uuid");
  mock_image_meta.lineage.push_back({});  // must be cleared by load
  C_SaferCond ctx;
  mock_image_meta.load(&ctx);
  ASSERT_EQ(0, ctx.wait());
  ASSERT_FALSE(mock_image_meta.resync_requested);
  ASSERT_TRUE(mock_image_meta.lineage.empty());
}

TEST_F(TestMockMirrorSnapshotImageMeta, LoadWithLineage) {
  librbd::ImageCtx* image_ctx;
  ASSERT_EQ(0, open_image(m_image_name, &image_ctx));
  MockTestImageCtx mock_image_ctx(*image_ctx);

  InSequence seq;
  expect_metadata_get(mock_image_ctx, "mirror uuid",
                      "{\"resync_requested\": false, \"lineage\": ["
                      "{\"from_mirror_uuid\": \"uuid-a\", \"from_snap_id\": 7,"
                      " \"local_snap_id\": 9, \"promote_snap_id\": 11,"
                      " \"forced\": true, \"timestamp\": 1234,"
                      " \"snap_seqs\": {\"3\": 4, \"5\": 6}}]}", 0);

  MockImageMeta mock_image_meta(&mock_image_ctx, "mirror uuid");
  C_SaferCond ctx;
  mock_image_meta.load(&ctx);
  ASSERT_EQ(0, ctx.wait());

  LineageEntry expected;
  expected.from_mirror_uuid = "uuid-a";
  expected.from_snap_id = 7;
  expected.local_snap_id = 9;
  expected.promote_snap_id = 11;
  expected.forced = true;
  expected.timestamp = 1234;
  expected.snap_seqs = {{3, 4}, {5, 6}};
  ASSERT_EQ(std::vector<LineageEntry>{expected}, mock_image_meta.lineage);
}

TEST_F(TestMockMirrorSnapshotImageMeta, LoadCorruptLineage) {
  librbd::ImageCtx* image_ctx;
  ASSERT_EQ(0, open_image(m_image_name, &image_ctx));
  MockTestImageCtx mock_image_ctx(*image_ctx);

  InSequence seq;
  expect_metadata_get(mock_image_ctx, "mirror uuid",
                      "{\"resync_requested\": false, \"lineage\": ["
                      "{\"from_mirror_uuid\": \"uuid-a\"}]}", 0);

  MockImageMeta mock_image_meta(&mock_image_ctx, "mirror uuid");
  C_SaferCond ctx;
  mock_image_meta.load(&ctx);
  ASSERT_EQ(-EBADMSG, ctx.wait());
}

TEST_F(TestMockMirrorSnapshotImageMeta, SaveWithLineage) {
  librbd::ImageCtx* image_ctx;
  ASSERT_EQ(0, open_image(m_image_name, &image_ctx));
  MockTestImageCtx mock_image_ctx(*image_ctx);

  InSequence seq;
  expect_metadata_set(mock_image_ctx, "mirror uuid",
                      "{\"lineage\":[{\"forced\":false,"
                      "\"from_mirror_uuid\":\"uuid-a\",\"from_snap_id\":7,"
                      "\"local_snap_id\":9,\"promote_snap_id\":11,"
                      "\"snap_seqs\":{\"3\":4},\"timestamp\":1234}],"
                      "\"resync_requested\":false}", 0);

  MockImageMeta mock_image_meta(&mock_image_ctx, "mirror uuid");
  LineageEntry entry;
  entry.from_mirror_uuid = "uuid-a";
  entry.from_snap_id = 7;
  entry.local_snap_id = 9;
  entry.promote_snap_id = 11;
  entry.timestamp = 1234;
  entry.snap_seqs = {{3, 4}};
  mock_image_meta.add_lineage(entry);

  C_SaferCond ctx;
  mock_image_meta.save(&ctx);
  ASSERT_EQ(0, ctx.wait());
}

TEST_F(TestMockMirrorSnapshotImageMeta, AddLineageTrimsOldest) {
  librbd::ImageCtx* image_ctx;
  ASSERT_EQ(0, open_image(m_image_name, &image_ctx));
  MockTestImageCtx mock_image_ctx(*image_ctx);

  MockImageMeta mock_image_meta(&mock_image_ctx, "mirror uuid");
  for (uint64_t i = 0; i < MockImageMeta::MAX_LINEAGE_ENTRIES + 3; ++i) {
    LineageEntry entry;
    entry.promote_snap_id = i;
    mock_image_meta.add_lineage(entry);
  }
  ASSERT_EQ(MockImageMeta::MAX_LINEAGE_ENTRIES, mock_image_meta.lineage.size());
  ASSERT_EQ(3U, mock_image_meta.lineage.front().promote_snap_id);
  ASSERT_EQ(MockImageMeta::MAX_LINEAGE_ENTRIES + 2,
            mock_image_meta.lineage.back().promote_snap_id);
}

TEST_F(TestMockMirrorSnapshotImageMeta, Save) {
  librbd::ImageCtx* image_ctx;
  ASSERT_EQ(0, open_image(m_image_name, &image_ctx));
  MockTestImageCtx mock_image_ctx(*image_ctx);

  InSequence seq;
  expect_metadata_set(mock_image_ctx, "mirror uuid",
                      "{\"resync_requested\":true}", 0);

  MockImageMeta mock_image_meta(&mock_image_ctx, "mirror uuid");
  mock_image_meta.resync_requested = true;

  C_SaferCond ctx;
  mock_image_meta.save(&ctx);
  ASSERT_EQ(0, ctx.wait());

  // should have sent image-update notification
  ASSERT_TRUE(image_ctx->state->is_refresh_required());
}

TEST_F(TestMockMirrorSnapshotImageMeta, SaveError) {
  librbd::ImageCtx* image_ctx;
  ASSERT_EQ(0, open_image(m_image_name, &image_ctx));
  MockTestImageCtx mock_image_ctx(*image_ctx);

  InSequence seq;
  expect_metadata_set(mock_image_ctx, "mirror uuid",
                      "{\"resync_requested\":false}", -EINVAL);

  MockImageMeta mock_image_meta(&mock_image_ctx, "mirror uuid");

  C_SaferCond ctx;
  mock_image_meta.save(&ctx);
  ASSERT_EQ(-EINVAL, ctx.wait());
}

} // namespace snapshot
} // namespace mirror
} // namespace librbd
