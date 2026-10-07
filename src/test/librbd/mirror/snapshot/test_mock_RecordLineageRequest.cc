// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#include "test/librbd/test_mock_fixture.h"
#include "test/librbd/test_support.h"
#include "test/librbd/mock/MockImageCtx.h"
#include "librbd/mirror/GetUuidRequest.h"
#include "librbd/mirror/snapshot/ImageMeta.h"
#include "librbd/mirror/snapshot/RecordLineageRequest.h"

namespace librbd {

namespace {

struct MockTestImageCtx : public MockImageCtx {
  explicit MockTestImageCtx(librbd::ImageCtx& image_ctx) : MockImageCtx(image_ctx) {
  }
};

} // anonymous namespace

namespace mirror {

template <>
struct GetUuidRequest<MockTestImageCtx> {
  std::string* mirror_uuid = nullptr;
  Context* on_finish = nullptr;
  static GetUuidRequest* s_instance;
  static GetUuidRequest *create(librados::IoCtx& io_ctx,
                                std::string* mirror_uuid, Context* on_finish) {
    ceph_assert(s_instance != nullptr);
    s_instance->mirror_uuid = mirror_uuid;
    s_instance->on_finish = on_finish;
    return s_instance;
  }

  MOCK_METHOD0(send, void());

  GetUuidRequest() {
    s_instance = this;
  }
};

GetUuidRequest<MockTestImageCtx>* GetUuidRequest<MockTestImageCtx>::s_instance = nullptr;

namespace snapshot {

// The request owns (and deletes) the ImageMeta it creates, so the mock itself
// must be heap-allocated; expectations live on a separate static object.
template <>
struct ImageMeta<MockTestImageCtx> {
  static constexpr size_t MAX_LINEAGE_ENTRIES = 8;

  struct Expectations {
    MOCK_METHOD2(load, void(ImageMeta*, Context*));
    MOCK_METHOD2(save, void(ImageMeta*, Context*));
    Expectations() { s_expectations = this; }
    ~Expectations() { s_expectations = nullptr; }
  };
  static Expectations* s_expectations;

  std::string mirror_uuid;
  bool resync_requested = false;
  std::vector<LineageEntry> lineage;

  static ImageMeta* create(MockTestImageCtx* image_ctx,
                           const std::string& mirror_uuid) {
    auto image_meta = new ImageMeta();
    image_meta->mirror_uuid = mirror_uuid;
    return image_meta;
  }

  void load(Context* on_finish) {
    ceph_assert(s_expectations != nullptr);
    s_expectations->load(this, on_finish);
  }
  void save(Context* on_finish) {
    ceph_assert(s_expectations != nullptr);
    s_expectations->save(this, on_finish);
  }
  void add_lineage(const LineageEntry& entry) {
    lineage.push_back(entry);
  }
};

ImageMeta<MockTestImageCtx>::Expectations*
ImageMeta<MockTestImageCtx>::s_expectations = nullptr;

} // namespace snapshot
} // namespace mirror
} // namespace librbd

// template definitions
#include "librbd/mirror/snapshot/RecordLineageRequest.cc"
template class librbd::mirror::snapshot::RecordLineageRequest<librbd::MockTestImageCtx>;

namespace librbd {
namespace mirror {
namespace snapshot {

using ::testing::_;
using ::testing::InSequence;
using ::testing::Invoke;

class TestMockMirrorSnapshotRecordLineageRequest : public TestMockFixture {
public:
  typedef RecordLineageRequest<MockTestImageCtx> MockRecordLineageRequest;
  typedef GetUuidRequest<MockTestImageCtx> MockGetUuidRequest;
  typedef ImageMeta<MockTestImageCtx> MockImageMeta;

  void expect_get_uuid(MockTestImageCtx& mock_image_ctx,
                       MockGetUuidRequest& mock_get_uuid_request,
                       const std::string& mirror_uuid, int r) {
    EXPECT_CALL(mock_get_uuid_request, send())
      .WillOnce(Invoke([&mock_image_ctx, &mock_get_uuid_request, mirror_uuid,
                        r]() {
          *mock_get_uuid_request.mirror_uuid = mirror_uuid;
          mock_image_ctx.image_ctx->op_work_queue->queue(
            mock_get_uuid_request.on_finish, r);
        }));
  }

  // state captured from the heap-allocated mock before the request deletes it
  struct Captured {
    std::string mirror_uuid;
    std::vector<LineageEntry> lineage;
  };

  void expect_load(MockTestImageCtx& mock_image_ctx,
                   MockImageMeta::Expectations& expectations,
                   const std::vector<LineageEntry>& existing, int r) {
    EXPECT_CALL(expectations, load(_, _))
      .WillOnce(Invoke([&mock_image_ctx, existing, r]
                       (MockImageMeta* image_meta, Context* ctx) {
          if (r >= 0) {
            image_meta->lineage = existing;
          }
          mock_image_ctx.image_ctx->op_work_queue->queue(ctx, r);
        }));
  }

  void expect_save(MockTestImageCtx& mock_image_ctx,
                   MockImageMeta::Expectations& expectations,
                   Captured* captured, int r) {
    EXPECT_CALL(expectations, save(_, _))
      .WillOnce(Invoke([&mock_image_ctx, captured, r]
                       (MockImageMeta* image_meta, Context* ctx) {
          if (captured != nullptr) {
            captured->mirror_uuid = image_meta->mirror_uuid;
            captured->lineage = image_meta->lineage;
          }
          mock_image_ctx.image_ctx->op_work_queue->queue(ctx, r);
        }));
  }

  LineageEntry make_entry() {
    LineageEntry entry;
    entry.from_mirror_uuid = "uuid-a";
    entry.from_snap_id = 7;
    entry.local_snap_id = 9;
    entry.promote_snap_id = 11;
    entry.forced = true;
    entry.snap_seqs = {{3, 4}};
    entry.timestamp = 1234;
    return entry;
  }
};

TEST_F(TestMockMirrorSnapshotRecordLineageRequest, Success) {
  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));
  MockTestImageCtx mock_image_ctx(*ictx);
  expect_op_work_queue(mock_image_ctx);

  InSequence seq;
  MockGetUuidRequest mock_get_uuid_request;
  expect_get_uuid(mock_image_ctx, mock_get_uuid_request, "uuid-b", 0);
  MockImageMeta::Expectations mock_image_meta;
  Captured captured;
  expect_load(mock_image_ctx, mock_image_meta, {}, 0);
  expect_save(mock_image_ctx, mock_image_meta, &captured, 0);

  auto entry = make_entry();
  C_SaferCond ctx;
  auto req = MockRecordLineageRequest::create(&mock_image_ctx, entry, &ctx);
  req->send();
  ASSERT_EQ(0, ctx.wait());
  ASSERT_EQ("uuid-b", captured.mirror_uuid);
  ASSERT_EQ(std::vector<LineageEntry>{entry}, captured.lineage);
}

TEST_F(TestMockMirrorSnapshotRecordLineageRequest, AppendsToExisting) {
  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));
  MockTestImageCtx mock_image_ctx(*ictx);
  expect_op_work_queue(mock_image_ctx);

  LineageEntry older = make_entry();
  older.promote_snap_id = 2;

  InSequence seq;
  MockGetUuidRequest mock_get_uuid_request;
  expect_get_uuid(mock_image_ctx, mock_get_uuid_request, "uuid-b", 0);
  MockImageMeta::Expectations mock_image_meta;
  Captured captured;
  expect_load(mock_image_ctx, mock_image_meta, {older}, 0);
  expect_save(mock_image_ctx, mock_image_meta, &captured, 0);

  auto entry = make_entry();
  C_SaferCond ctx;
  auto req = MockRecordLineageRequest::create(&mock_image_ctx, entry, &ctx);
  req->send();
  ASSERT_EQ(0, ctx.wait());
  ASSERT_EQ((std::vector<LineageEntry>{older, entry}), captured.lineage);
}

TEST_F(TestMockMirrorSnapshotRecordLineageRequest, NoExistingImageMeta) {
  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));
  MockTestImageCtx mock_image_ctx(*ictx);
  expect_op_work_queue(mock_image_ctx);

  InSequence seq;
  MockGetUuidRequest mock_get_uuid_request;
  expect_get_uuid(mock_image_ctx, mock_get_uuid_request, "uuid-b", 0);
  MockImageMeta::Expectations mock_image_meta;
  expect_load(mock_image_ctx, mock_image_meta, {}, -ENOENT);
  expect_save(mock_image_ctx, mock_image_meta, nullptr, 0);

  C_SaferCond ctx;
  auto req = MockRecordLineageRequest::create(&mock_image_ctx, make_entry(),
                                              &ctx);
  req->send();
  ASSERT_EQ(0, ctx.wait());
}

TEST_F(TestMockMirrorSnapshotRecordLineageRequest, GetUuidError) {
  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));
  MockTestImageCtx mock_image_ctx(*ictx);
  expect_op_work_queue(mock_image_ctx);

  InSequence seq;
  MockGetUuidRequest mock_get_uuid_request;
  expect_get_uuid(mock_image_ctx, mock_get_uuid_request, "", -EINVAL);

  C_SaferCond ctx;
  auto req = MockRecordLineageRequest::create(&mock_image_ctx, make_entry(),
                                              &ctx);
  req->send();
  ASSERT_EQ(-EINVAL, ctx.wait());
}

TEST_F(TestMockMirrorSnapshotRecordLineageRequest, LoadError) {
  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));
  MockTestImageCtx mock_image_ctx(*ictx);
  expect_op_work_queue(mock_image_ctx);

  InSequence seq;
  MockGetUuidRequest mock_get_uuid_request;
  expect_get_uuid(mock_image_ctx, mock_get_uuid_request, "uuid-b", 0);
  MockImageMeta::Expectations mock_image_meta;
  expect_load(mock_image_ctx, mock_image_meta, {}, -EBADMSG);

  C_SaferCond ctx;
  auto req = MockRecordLineageRequest::create(&mock_image_ctx, make_entry(),
                                              &ctx);
  req->send();
  ASSERT_EQ(-EBADMSG, ctx.wait());
}

TEST_F(TestMockMirrorSnapshotRecordLineageRequest, SaveError) {
  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));
  MockTestImageCtx mock_image_ctx(*ictx);
  expect_op_work_queue(mock_image_ctx);

  InSequence seq;
  MockGetUuidRequest mock_get_uuid_request;
  expect_get_uuid(mock_image_ctx, mock_get_uuid_request, "uuid-b", 0);
  MockImageMeta::Expectations mock_image_meta;
  expect_load(mock_image_ctx, mock_image_meta, {}, 0);
  expect_save(mock_image_ctx, mock_image_meta, nullptr, -EIO);

  C_SaferCond ctx;
  auto req = MockRecordLineageRequest::create(&mock_image_ctx, make_entry(),
                                              &ctx);
  req->send();
  ASSERT_EQ(-EIO, ctx.wait());
}

} // namespace snapshot
} // namespace mirror
} // namespace librbd
