# rbd-mirror peer re-link / push mirroring — 未完成工作汇总

> 基于 `wip-rbd-mirror-peer-relink` 分支（HEAD 91dbee4，base main 2d0cd8a）。
> 已完成部分与设计背景见 `PHASE0_PEER_RELINK.md`（Phase 0 设计 + 基线实测）和
> `PUSH_MIRRORING_ANALYSIS.md`（Phase 1/2 push 改动地图）。本文只列**尚未做的事**。

---

## 0. 当前状态一览

| 项目 | 状态 | 位置 |
|---|---|---|
| 基线测量（T1 + T2 三变体） | 完成 | `PHASE0_PEER_RELINK.md` §1.3a，`~/mirror-lab/run/` |
| PR-1：ImageMeta `lineage` + `RecordLineageRequest` + `PromoteRequest` 记录血统 | 完成（commit ceefce9） | `src/librbd/mirror/snapshot/{ImageMeta,RecordLineageRequest,PromoteRequest}.*` |
| PR-2：Replayer RELINK_FAST（n == m） | 完成（commit 91dbee4，三集群实测 7.7 MiB vs 122 MiB） | `src/tools/rbd_mirror/image_replayer/snapshot/Replayer.cc` `validate_lineage()` |
| PR-2：RELINK_FORWARD / RELINK_ROLLBACK | **未做**（`Replayer.cc:970` 留有 TODO） | — |
| PR-3：promote 快照 prune 豁免 + `--check-peers / --require-latest` | **未做** | — |
| PR-4：qa 3 集群 workunit + mstart 端口 + 文档 | **未做** | — |
| 附带 bug：`Mirror::update_pool_replayers()` 同步 `shut_down()` 卡死 | **未做** | `src/tools/rbd_mirror/Mirror.cc:722` |
| 设计 RFC（ceph-devel / CDM） | **未做** | — |
| Phase 1/2 push mirroring | 仅分析，未动手 | `PUSH_MIRRORING_ANALYSIS.md` |

---

## 1. Phase 0 剩余（按优先级）

### 1.1 Replayer：RELINK_FORWARD / RELINK_ROLLBACK（PR-2 续）

现状：`validate_lineage()` 只接受 `from_snap_id == 本地 primary_snap_id`，其余一律
`-EEXIST "lineage mismatch: ..."`。

- [ ] **RELINK_FORWARD（n < m，C 落后）**
  - B 上仍存在 `primary_mirror_uuid == A && primary_snap_id == n` 的 non-primary 快照
    → 以该快照为 delta 起点、`B.end` 为终点做 `ImageCopyRequest`
  - 否则 → `RESYNC_REQUIRED`，文案 `"lineage unverifiable (local=A@n, remote lineage=A@m): resync required"`
  - 注意 §1.5：B 作为 non-primary 时只保留 1 个 non-primary 快照，FORWARD 精确路径窗口很窄，
    需在 RFC 中明确该限制
- [ ] **RELINK_ROLLBACK（n > m，C 领先）**
  - C 本地仍有对应 A@m 的 non-primary 快照 → 回滚（复用 `PromoteRequest` force 路径的
    rollback 逻辑 / `m_rollback_snap_id`）到 A@m 后按 FAST 处理
  - 否则 → `RESYNC_REQUIRED`
  - 回滚前要求本地无 watcher（防止在线写入被丢弃）；status description 明示丢弃的数据范围
- [ ] **snap_seqs 组合（§3.4）**：目前 FAST 路径按名重建用户快照映射；FORWARD/ROLLBACK 需按
  `B.anchor.snap_seqs ⋈ C.last.snap_seqs`（以 A.snap 为键）组合，并把 `CEPH_NOSNAP` 解释为
  lineage 的 `local_snap_id`。lineage 条目已携带 `snap_seqs` 副本（`PromoteRequest.cc:70`），
  锚点快照被 prune 后可从中读取
- [ ] **T7 拒绝路径**：确认 B 未真正 promote（无 PRIMARY 快照 / `promote_snap_id` 不可追溯）时
  拒绝 relink 且本地零写入；现有实现是否覆盖需补 mock 测试
- [ ] **status description**：等待下一个调度快照时上报
  `"relinked to <B>, waiting for next mirror snapshot"`（§3.5/§3.8），目前只有日志
- [ ] mock 测试补齐：FORWARD 成功 / FORWARD 锚点已 prune / ROLLBACK 成功 / ROLLBACK 本地已 prune /
  T7 拒绝（现有 4 个：成功 / 不匹配 / 无 lineage / 锚点已删）
- [ ] 复核与 #55803 类竞态：验证须在 `scan_remote` 持锁窗口内完成，锚点在窗口内被 prune 时
  走现有 "remote start snapshot was deleted" 兜底

### 1.2 ImageMeta / lineage 生命周期

- [ ] `lineage` 数组有界 FIFO（设计为最近 8 条）：确认 `ImageMeta::add_lineage()`
  （`ImageMeta.cc:80`）已截断，补单测
- [ ] T11：`rbd mirror image resync` 路径不误清 lineage（`resync_requested` 写回时保留数组）
- [ ] `mirror image disable` 时 lineage 随 `.rbd_mirror.<uuid>` key 一起删除——确认现有删除路径覆盖
- [ ] 老 daemon（无 lineage）兼容：T8 退化路径——窗口内走快速路径（B 上仍有 `primary_mirror_uuid==A`
  的 non-primary 快照），否则 `RESYNC_REQUIRED`

### 1.3 PR-3：prune 豁免 + 升主预检

- [ ] `CreatePrimaryRequest.cc:181-185`：promote 快照改为 "直到 `mirror_peer_uuids` 全部 unlink
  才参与 `rbd_mirroring_max_mirroring_snapshots` 轮转"
- [ ] 新配置 `rbd_mirroring_promote_snapshot_retain_until_unlinked`（默认 true），
  `src/common/options/rbd.yaml.in`；异常备端永不 unlink 时仍以现有 max 上限兜底
- [ ] `rbd mirror image promote --force --check-peers`：对本池所有已配置 peer 查询该镜像最后
  complete non-primary 快照的 `primary_snap_id`，发现更领先者警告并列出；不可达 peer 标 `unknown`
- [ ] `--require-latest`：存在更领先者则拒绝，提示去该站点 promote
- [ ] `rbd mirror pool promote` 同样支持两个 flag（只能警告，不能拆站升主）
- [ ] 改动点：`src/librbd/api/Mirror.cc`、`src/tools/rbd/action/MirrorImage.cc`、`MirrorPool.cc`
- [ ] 测试：T12（`--require-latest` 拒绝）、T13（peer 不可达降级 warning）、T15（豁免生效直到 unlink）

### 1.4 附带 bug：daemon 被不可达的旧主端卡死（可独立提 PR / tracker）

- [ ] `Mirror::update_pool_replayers()`（`Mirror.cc:722`，`// TODO: make async`）在 `m_lock` 下
  同步调用被移除 peer 的 `PoolReplayer::shut_down()`；旧主端不可达时永久阻塞在 RADOS 操作上
  （`rados_mon_op_timeout` 默认 0），新 peer 的 PoolReplayer 永远不建，SIGTERM 无效
- [ ] 修法二选一：异步 shut_down（把 PoolReplayer 移出 map 后在 `m_lock` 外销毁）或为被移除
  peer 的 RADOS 连接设置超时
- [ ] 三个 T2 变体 100% 复现，先在 tracker 建 bug 再提 PR
- [ ] 测试 T16：旧主端不可达时 `peer remove` + `peer add`，daemon 不卡死，新 PoolReplayer 在刷新间隔内建立

### 1.5 PR-4：qa + 工具 + 文档

- [ ] `qa/workunits/rbd/rbd_mirror_helpers.sh`：支持第三个集群（cluster3）
- [ ] 新 `qa/workunits/rbd/rbd_mirror_relink.sh`：T1-T9 场景，每条用 `compare_images` +
  perf counter `l_rbd_mirror_snapshot_sync_bytes`
- [ ] `src/mstart.sh` 基准端口 6800 → 6900（复活 MrFreezeex 的 commit，避免三集群端口冲突）
- [ ] T10：现有 `rbd_mirror.sh` / `rbd_mirror_bootstrap.sh` 全套零回归
- [ ] `doc/rbd/rbd-mirroring.rst`：新增 "Peer re-link after failover" 章节 + 预检工具说明 +
  "换绑后首个增量等到下一个调度快照" 的运维提示
- [ ] 把 `~/mirror-lab/{mirror3.sh,t1_baseline.sh,t2_force.sh,t1_relink.sh}` 整理进 qa 或
  `src/test/rbd_mirror/` 旁的脚本目录（目前在仓库外）

### 1.6 RFC 与上游沟通

- [ ] ceph-devel 邮件 + CDM 议题，附 `PHASE0_PEER_RELINK.md` §3 设计与 §1.3a 基线数据
- [ ] 点名 idryomov / trociny review；联系 MrFreezeex 复用 #43359 的三集群 qa 代码
- [ ] RFC 中明确：FORWARD 精确路径窗口窄、ROLLBACK 丢弃 C 领先数据是正确语义、
  Δ 退化路径不做（锚点 prune 即 resync）
- [ ] 复测基线：同一 T1 的 before/after 写进 PR 描述

### 1.7 提交整理

- [ ] 本分支两个 commit 拆成 PR-1 / PR-2 分别开 PR（每个 < 500 行），PR-1 不改行为先合
- [ ] `PHASE0_PEER_RELINK.md` / `PUSH_MIRRORING_ANALYSIS.md` / 本文不随 PR 提交，仅在 fork 分支保留
- [ ] `.clangd` 不入库

---

## 2. Phase 1 / Phase 2：push-based snapshot mirroring（未动手）

详细改动地图见 `PUSH_MIRRORING_ANALYSIS.md` §2，此处只列任务清单。

### 2.1 动手前的设计决策（§5）

- [ ] TX-only peer 才走 push，RX/RX_TX 维持 pull（建议）
- [ ] PoC 复制 `PushReplayer` vs 抽象 src/dst：PoC 阶段复制，上游化前重构为 src/dst 参数化
- [ ] 删除传播机制（纯 push 部署对端无 daemon）
- [ ] `resync` / `demote` 等命令在 push 下的目标集群语义
- [ ] dev@ceph.io 发提案帖，与 Ilya Dryomov 对齐 direction 语义

### 2.2 阶段 A：零代码数据面验证

- [ ] 两集群 `export-diff | import-diff` 手工推送，确认 diff 语义与 fast-diff 依赖

### 2.3 阶段 B：最小 daemon PoC

- [ ] `Types.h` `PeerSpec` 加 direction；`ClusterWatcher::read_pool_peers()` 放行 TX peer
- [ ] `PoolReplayer` 透传 `push_mode`
- [ ] `BootstrapRequest` push 分支：`is_local_primary()` 继续；远端缺镜像 → 新
  `CreateRemoteImageRequest`（镜像 `snapshot/CreateLocalImageRequest` 改 ioctx）
- [ ] `PrepareReplayRequest` push 化：检查远端 non-primary / linked / resync 标记
- [ ] `snapshot::PushReplayer`：按 §2.4 表逐函数反转 local/remote
- [ ] peer 配置 PoC 用 `peer add` rx-tx + `peer set mon-host/key-file/direction tx-only` 手工构造
- [ ] 验证：基础推送 / 增量只传 delta / 断点续传 / unlink_peer 回收 / 远端 promote 后推送停止 /
  新建·删除·禁用镜像

### 2.4 阶段 C：工程化

- [ ] 控制面补齐：`peer_bootstrap_import()` 允许 TX、cls `peer_add` TX 限制、`--direction tx-only`
- [ ] `ImageDeleter` push 语义的远端副本清理
- [ ] `MirrorStatusUpdater` 写入目标 ioctx 按 push/pull 决定；`rbd mirror image/pool status` 展示核对
- [ ] `test_mock_PushReplayer.cc` 对称 mock 测试
- [ ] qa `RBD_MIRROR_MODE=push` 或新 `rbd_mirror_push.sh`，接入 teuthology `rbd:mirror`
- [ ] 多 daemon HA + 多 peer；promote/demote/resync/split-brain 全流程
- [ ] Phase 2 fan-out：hub 对每个目标做 Phase 0 §3.2 验证（local/remote 互换）

---

## 3. 环境备忘

- 构建：`./do_cmake.sh -DWITH_TESTS=ON -DWITH_RADOSGW=OFF -DWITH_MGR_DASHBOARD_FRONTEND=OFF
  -DWITH_CEPHFS=OFF -DWITH_RBD_RWL=OFF -DWITH_SYSTEM_BOOST=OFF`，
  `ninja -j4 ceph-mon ceph-osd ceph-mgr ceph rbd rados rbd-mirror ceph-authtool ceph-conf
  monmaptool cls_rbd unittest_rbd_mirror ceph_test_cls_rbd`（约 95 min）
- 单测：`build/bin/unittest_rbd_mirror`（当前 490 全绿）、`unittest_librbd` 中 ImageMeta /
  RecordLineage / Promote 共 22 个
- 三集群：`MON=1 OSD=1 MDS=0 RGW=0 ../src/mstart.sh clusterN -n --without-dashboard`，
  实验脚本 `~/mirror-lab/`（在仓库外，见 §1.5 待整理项）
- WSL2：`.wslconfig` `memory=12GB swap=8GB processors=16`；三集群运行态约 2.6 GB
