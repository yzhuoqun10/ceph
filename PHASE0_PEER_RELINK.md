# Phase 0: Peer Re-link without Resync（安全的 peer 重链接）

> 基于 ceph main 2d0cd8a。配套文档：PUSH_MIRRORING_ANALYSIS.md（Phase 1/2 push 改动地图）。
> 本文档是 "push-based snapshot mirroring with fan-out" 提案的前置阶段，独立可交付。

---

## 0. 一句话目标

让 non-primary 镜像在"它所跟随的 primary 换了集群"之后，能验证新 primary 的血统并续传增量，
而不是报 `local image linked to unknown peer` 后被迫全量 resync。

## 1. 问题

### 1.1 场景

```
A ──→ B
A ──→ C           A primary，B/C 各自独立从 A 拉
# A demote（有序）或 A 宕机 force promote B
B ──→ C           ← C 的 replayer 在这一步死掉
```

### 1.2 死在哪

`src/tools/rbd_mirror/image_replayer/snapshot/Replayer.cc:592-605`：
C 本地最后一个 non-primary 快照 `primary_mirror_uuid == A`，而 `remote_mirror_uuid == B`
→ `-EEXIST "local image linked to unknown peer"`，注释 `// TODO support multiple peers`。
唯一出路 `rbd mirror image resync` = 全量拷贝。

### 1.3 上游证据

- CERN PR #43359（rbd-mirror multipeer，2021）在三集群实测到此故障；trociny 定性
  "logic was developed with one peer only in mind ... might need to store some additional
  information in the tag/snapshot to make the peer change safe"。PR 因作者离开与 review
  停滞于 2023 关闭，非技术否决。
- cephfs #61978 / PR #52456（push fan-out）同样因 review 停滞于 2025-11 关闭。
- #16540（多 peer mesh）2016 年至今 New。

### 1.3a 本机基线实测（2026-10-05，未改代码的 main 2d0cd8a，3 × mstart 集群）

脚本：`~/mirror-lab/{mirror3.sh,t1_baseline.sh,t2_force.sh}`；原始输出 `~/mirror-lab/run/t{1,2}_*.out`。
镜像 256 MiB，已分配约 122 MiB；failover 后 B 只写了 8 MiB 增量。

| 场景 | C 换绑后状态 | 恢复手段 | C 实际需要 | C 实际传输 | 放大 | 人工步骤* |
|---|---|---|---|---|---|---|
| T1 有序（A demote → B promote） | `up+error: local image linked to unknown peer` | resync | 8 MiB | 122 MiB | 15× | 6 |
| T2 force，n == m | 同上（含 daemon 重启后） | resync | 8 MiB | 122 MiB | 15× | 6 |
| T2 force，C 落后 (n < m) | 同上 | resync | 24 MiB | 130 MiB | 5.4× | 6 |
| T2 force，C 领先 (n > m) | 同上 | resync | 8 MiB | 125 MiB | 15× | 6 |

\* 单镜像 × 单个被换绑备端：promote / peer remove / peer add / (daemon 重启) / 触发快照 / resync。
每多一个备端，后 4-5 步重复一遍。

实测顺带确认的事实：

1. **n == m 在有序 failover 下必然成立**：demote 快照在 B、C 上均为 `demoted ... <A>:7 copied`，
   RELINK_FAST 覆盖全部有序路径。
2. **锚点确实消失**：force promote 后 ~15 s 内 B 上只剩 promote 快照（`snap ls` 仅 1 项），
   A 的 non-primary 快照已被 B 自己的 daemon prune。§1.5 的结论是实测而非推断。
3. **peer uuid 注册时序（§3.5）**：换绑后 C 对 B 的所有既有快照报
   `skipping remote snapshot due to missing mirror peer`，直到 B 产生下一个快照才有 end 点。
4. **C 领先的数据静默丢失**：n > m 场景下 C 持有 A 已提交但 B 未收到的 16 MiB，resync 后无任何
   警告地被 B 的版本覆盖——§3.7 预检工具的直接动机。
5. **新发现：幸存备端的 rbd-mirror 在 peer 换绑时被已死的旧主端卡死。**
   `Mirror::update_pool_replayers()` 在 `m_lock` 下同步调用被移除 peer 的 `PoolReplayer::shut_down()`
   （`Mirror.cc:722`，注释 `// TODO: make async`）；旧主端不可达时该调用在 RADOS 操作上永久阻塞
   （`rados_mon_op_timeout` 默认 0），**新 peer 的 PoolReplayer 永远不会被创建**，SIGTERM 也无法停止
   daemon，只能 `kill -9` 后重启。三个 T2 变体 100% 复现。这是 pull 拓扑下非计划 failover 的
   独立运维风险，与血统问题正交；修复（异步 shut_down 或带超时）应作为 Phase 0 的附带 PR 或单独 bug 提交。

### 1.4 为什么元数据层面可解

元数据分散记账、无全局视图，但有一个跨站点可比的锚：**pool 级 mirror_uuid**（peer uuid 是
pool 内局部值，不可跨站比对）。

| 位置 | 字段 | 含义 |
|---|---|---|
| C 最后 complete non-primary 快照 | `primary_mirror_uuid=A`, `primary_snap_id=n`, `snap_seqs{A→C}` | "C 的数据 == A@n" |
| B promote 前最后 complete non-primary 快照 | `primary_mirror_uuid=A`, `primary_snap_id=m`, `snap_seqs{A→B}` | "B 在 A@m 处接管" |

`complete=true` 是原子提交点 ⇒ 若 `n == m`，B@(A@m) 与 C@(A@m) 数据相同，无需逐对象校验；
C 缺的只是 B promote 之后的增量 Δ。

### 1.5 为什么锚点不能靠快照（本阶段最重要的设计约束）

快照链被激进修剪：
- B 作为 non-primary 时，non-primary 快照 `mirror_peer_uuids={}`（`CreateNonPrimaryRequest.cc:185-191`），
  Replayer 每次 scan 只豁免最新一个 complete 快照，其余全部 prune（`Replayer.cc:515-528, 578-588`）
  → B 任何时刻只保留 1 个 non-primary 快照。
- B promote 后（双向部署 B 自己也跑 daemon），下次 scan 时 promote 快照成为新起点，那个 A@m 锚点
  失去豁免被 prune。
- B 成为 primary 后，`rbd_mirroring_max_mirroring_snapshots`（默认 3）轮转，promote 快照本身约
  3 个调度周期后也被 unlink + 删除（`CreatePrimaryRequest.cc:181-185`）。

结论：血统必须持久化到快照之外。

## 2. 范围

**做**
- snapshot 模式 replayer 的血统验证 + 重链接状态机
- 血统持久化到 image-meta（非快照）
- promote 快照 prune 豁免策略调整
- `promote --check-peers / --require-latest` 预检工具
- 手工 peer 换绑路径（`peer remove A` + `peer add B`）下零 resync
- 3 集群 qa 测试 + mock 单测

**不做（写进提案）**
- 单 daemon 同时监听多 peer（#43359 的 Replayer stack 多 peer 化）— Phase 2 push 是其替代路径
- journal 模式
- 自动 peer 发现/自动换绑
- A 回归后的 failback（现有两 peer 代码已覆盖，`Replayer.cc:694-714`）

## 3. 设计

### 3.1 血统持久化（必须项）

载体：`librbd/mirror/snapshot/ImageMeta`，已用 image header metadata key
`.rbd_mirror.<mirror_uuid>` 持久化 JSON（现存 `resync_requested`），不受快照轮转影响。

```json
".rbd_mirror.<B_mirror_uuid>": {
  "resync_requested": false,
  "lineage": [
    {"from_mirror_uuid": "A", "from_snap_id": m, "promote_snap_id": b_p, "forced": true, "ts": "..."}
  ]
}
```

- 写入点：`PromoteRequest` 成功后（有序与 force 两条路径），取 promote 前最后一个 complete
  non-primary 快照的 `(primary_mirror_uuid, primary_snap_id)`；force 路径即 `m_rollback_snap_id`
  对应项。
- `lineage` 有界（如最近 8 条），FIFO。
- 读取方（C）用 B 池的 mirror_uuid 组 key（`RemotePoolMeta` 已有该值）。
- 生命周期：`mirror image disable` 才删；需确认 `resync` 路径不误清（T11）。
- 兼容：老 daemon 不写 lineage；新 daemon 读不到时退化为 §3.2 快速路径或 RESYNC_REQUIRED。

### 3.2 血统验证算法（`scan_local_mirror_snapshots` 新分支 `validate_lineage`）

触发条件：`local.is_non_primary() && local.primary_mirror_uuid (=A) != remote_mirror_uuid (=B)`
且非 orphan。

```
1. 读 B 的 ImageMeta.lineage，找 from_mirror_uuid == A 的最近一条 → m, promote_snap_id
   （快速路径：若 B 上仍存在 primary_mirror_uuid==A 的 non-primary 快照，直接用它做精确锚点）
2. 确认 B 的镜像当前是 primary（存在 PRIMARY 快照且 promote_snap_id 可追溯），否则拒绝 relink（T7）
3. 比较 C 的 n 与 B 的 m：
   n == m → RELINK_FAST
   n <  m → RELINK_FORWARD   B 仍保留 primary_snap_id==n 的 non-primary 快照时可精确；否则 RESYNC_REQUIRED
   n >  m → RELINK_ROLLBACK  C 领先于新主：C 本地仍有 A@m 对应快照 → 回滚后按 FAST；否则 RESYNC_REQUIRED
4. 任一步失败 → RESYNC_REQUIRED（保守），错误文案可操作：
   "lineage unverifiable (local=A@n, remote lineage=A@m): resync required"
```

relink 成功前不写任何本地状态。

### 3.3 增量 Δ 的来源

实现时的简化：B 的 **promote 快照本身**就是数据锚点——有序 failover 下 B 在 demote 同步后未写入，
force 路径下回滚到锚点后才建 promote 快照，因此 `B@promote == B@anchor == A@m`，且 lineage 直接记录了
`promote_snap_id`。于是不需要 B 的 non-primary 锚点快照存活，也不需要 fast-diff 退化路径：

| promote 快照状态 | Δ 计算 |
|---|---|
| 仍存在（`rbd_mirroring_max_mirroring_snapshots` 轮转窗口内，§3.6 可延长） | 精确：`ImageCopyRequest(B.promote → B.end)` |
| 已被轮转删除 | `-EEXIST "lineage anchor pruned on remote: resync required"` |

`B.end` = anchor 之后第一个 `mirror_peer_uuids` 含 C 的 PRIMARY 快照（见 §3.5），非 promote 快照本身。

### 3.4 snap_seqs 组合

C 后续从 B 拉时 `deep_copy::SnapshotCopyRequest` 需知 B 的哪些用户快照 C 已有：

```
B.anchor.snap_seqs : A.snap → B.snap
C.last.snap_seqs   : A.snap → C.snap
按 A.snap join     : B.snap → C.snap
```

作为 C 新建首个 non-primary 快照（`primary_mirror_uuid=B`, `primary_snap_id=b_anchor`）的 `snap_seqs`
初值。锚点快照已 prune 时，B 侧 snap_seqs 从 lineage 条目附带的副本读取（lineage 条目需保存该 map）。

注意：non-primary 快照的 `snap_seqs` 把 primary 的 mirror 快照本身映射为 `CEPH_NOSNAP`
（实测 `{"6": 18446744073709551614}`），组合时须把 NOSNAP 解释为该 non-primary 快照自身
（即 lineage 的 `local_snap_id`）。

### 3.5 peer uuid 注册时序

B 的 promote 快照（`CREATE_PRIMARY_FLAG_IGNORE_EMPTY_PEERS`）不含 C 的 peer uuid（C 尚未 ping B）。
C 首次 scan 会 "skipping remote snapshot due to missing mirror peer"（`Replayer.cc:658-663`）。
处理：`remote_snap_id_end` 取 anchor 之后首个列出 C 的 PRIMARY 快照（通常是 rbd_support 下一次调度快照）。
文档明示：换绑后首个增量等到下一个调度快照。

### 3.6 promote 快照 prune 豁免

将 promote 快照从"按 `rbd_mirroring_max_mirroring_snapshots` 固定轮转"改为
"直到 `mirror_peer_uuids` 全部 unlink 为止不轮转"。两 peer 场景无影响；fan-out 下把精确 Δ 窗口从
"3 个周期"拉长到"所有备端完成重链接"。新增配置 `rbd_mirroring_promote_snapshot_retain_until_unlinked`
（默认 true），防异常备端永不 unlink 时用现有 max 上限兜底。

### 3.7 升主预检：`promote --check-peers / --require-latest`

C 领先 (n > m) 仅出现在 force promote（有序 failover 要求 demote 快照全同步，天然 n == m）。
此时选 B 升主会永久丢弃 C 上 A@m..A@n 的已提交数据，选最领先的备端严格更优。

```
rbd mirror image promote --force [--check-peers | --require-latest] pool/img
```

- `--check-peers`：对本池所有已配置 peer 查询该镜像最后 complete non-primary 快照的
  `primary_snap_id`，发现更领先者 → 警告并列出（建议默认开启）
- `--require-latest`：存在更领先者 → 拒绝，提示去该站点 promote
- 无 flag：现状，RTO 优先
- 限制：需 B 能连 C（凭据 + 网络）；查不到的 peer 标 `unknown` 不视为落后；
  `mirror pool promote` 时各镜像最领先 peer 可能不同，只能警告不能拆站升主

RELINK_ROLLBACK 仍必须实现（运维忽略警告或 peer 不可达时的兜底）。

### 3.8 错误/状态语义

| 现在 | Phase 0 后 |
|---|---|
| `-EEXIST "local image linked to unknown peer"` | RELINK_* 成功 → 继续 replay；RESYNC_REQUIRED → `-EEXIST "lineage unverifiable ...: resync required"`；等待下一个调度快照 → status description `"relinked to <B>, waiting for next mirror snapshot"` |

## 4. 改动清单

| 文件 | 改动 |
|---|---|
| `src/librbd/mirror/snapshot/ImageMeta.{h,cc}` | `lineage` 数组读写、有界 FIFO |
| `src/librbd/mirror/snapshot/PromoteRequest.cc` | promote 成功后追加 lineage 条目（含 snap_seqs 副本） |
| `src/librbd/mirror/snapshot/CreatePrimaryRequest.cc` | §3.6 prune 豁免 |
| `src/librbd/api/Mirror.cc` + `src/tools/rbd/action/MirrorImage.cc` / `MirrorPool.cc` | `--check-peers` / `--require-latest` |
| `src/tools/rbd_mirror/image_replayer/snapshot/Replayer.{h,cc}` | `validate_lineage()` / `relink_peer()` / 回滚 / snap_seqs 组合 |
| `src/tools/rbd_mirror/image_replayer/snapshot/StateBuilder.h` | relink anchor 字段 |
| `src/common/options/rbd.yaml.in` | 新配置项 |
| `src/test/rbd_mirror/image_replayer/snapshot/test_mock_Replayer.cc` | FAST/FORWARD/ROLLBACK/RESYNC/拒绝 五路径 |
| `src/test/librbd/mirror/snapshot/test_mock_ImageMeta.cc`, `test_mock_PromoteRequest.cc` | lineage 读写 |
| `qa/workunits/rbd/rbd_mirror_helpers.sh` + 新 `rbd_mirror_relink.sh` | 3 集群场景 |
| `src/mstart.sh` | 基准端口 6800→6900（复活 MrFreezeex 的 commit） |
| `src/tools/rbd_mirror/Mirror.cc` | （附带 bug）`update_pool_replayers` 对被移除 peer 的 `shut_down()` 改异步或加超时，避免被不可达的旧主端卡死 |
| `doc/rbd/rbd-mirroring.rst` | "Peer re-link after failover" + 预检工具 |

## 5. 测试矩阵

| # | 场景 | 期望 |
|---|---|---|
| T1 | 有序 failover（A demote → B promote），C 换绑到 B，n==m | RELINK_FAST，`sync_bytes` ≈ 增量 |
| T2 | force promote B，C 与 B 同切面 | RELINK_FAST |
| T3 | C 落后 (n<m)，B 保留 A@n | RELINK_FORWARD |
| T4 | C 落后，B 已 prune A@n | RESYNC_REQUIRED，文案可读 |
| T5 | C 领先 (n>m)，C 保留 A@m | RELINK_ROLLBACK，回滚后数据 == B@anchor |
| T6 | C 领先，C 已 prune | RESYNC_REQUIRED |
| T7 | B 未真正 promote（C 误换绑） | 拒绝 relink，本地零写入 |
| T8 | 老版本 daemon 创建的 promote（无 lineage） | 退化路径：窗口内快速路径 / 否则 RESYNC_REQUIRED |
| T9 | 换绑后 A 恢复，demote A 为 B 的从 | 现有 failback 不回归 |
| T10 | 现有 `rbd_mirror.sh` / `rbd_mirror_bootstrap.sh` 全套 | 零回归 |
| T11 | `rbd mirror image resync` 后 lineage 仍在 | 不被误清 |
| T12 | `--require-latest` 检测到 C 领先 | 拒绝升主并列出 C |
| T13 | `--check-peers` 时某 peer 不可达 | 降级 warning，标 unknown |
| T14 | 锚点已 prune 的 Δ 退化路径 | 数据一致，`sync_bytes` < 全量 |
| T15 | §3.6 豁免：3 周期后 promote 快照仍在，直到 C unlink 后才轮转 | 按预期 |
| T16 | 旧主端不可达时 `peer remove` + `peer add` | daemon 不卡死，新 PoolReplayer 在刷新间隔内建立 |

每条 qa 用例：`compare_images` + perf counter `l_rbd_mirror_snapshot_sync_bytes`。

## 6. 交付物与里程碑

1. **基线测量** ✅ 已完成（§1.3a）：T1 + T2 三变体，15× 放大、6 步/镜像/备端、daemon 卡死风险
2. **设计 RFC**：ceph-devel + CDM 议题，附本文 §3；点名 idryomov / trociny；联系 MrFreezeex 复用 qa 代码
3. **PR-1（小）** ✅ 本地完成（2026-10-05）：`ImageMeta` lineage 读写（json_spirit，向后兼容）、
   新 `RecordLineageRequest`、`PromoteRequest` 在 CREATE_PROMOTE_SNAPSHOT 后非致命地记录血统；
   单测 22 个（ImageMeta 10 / RecordLineage 6 / Promote 6）全绿，`unittest_rbd_mirror` 105 个无回归；
   三集群端到端：B promote 后 `.rbd_mirror.<B uuid>` 出现 lineage 条目
4. **PR-2** ✅ 本地完成（2026-10-07，commit 91dbee4，RELINK_FAST 路径）：`Replayer` 在本地起点快照
   链到非当前 remote 的 primary 时加载 remote image-meta（由 `StateBuilder` 持有）、按 `from_mirror_uuid`
   找 lineage、`from_snap_id == 本地 primary_snap_id` 则以 remote 的 promote 快照为 delta 起点，用户快照
   映射按名重建（复用 demote 路径）；不匹配/无记录/promote 快照已轮转/本地有未完成的旧主同步 → 带可操作
   文案的 `-EEXIST`。mock 测试 4 个（成功/不匹配/无 lineage/锚点已删），`unittest_rbd_mirror` 490 全绿。
   **三集群实测（`~/mirror-lab/t1_relink.sh`）：换绑后 C 传输 7.7 MiB（基线 122 MiB），数据一致，无 resync。**
   FORWARD / ROLLBACK 路径留待后续 PR。
5. **PR-3**：prune 豁免 + `--check-peers` 工具
6. **PR-4**：qa 3 集群 workunit + mstart 端口 + 文档
7. **复测基线**：同一 T1 的 before/after

## 7. 风险

| 风险 | 缓解 |
|---|---|
| reviewer 带宽（#43359/#52456 死因） | PR 切四段，PR-1 不改行为；RFC 先行；每 PR < 500 行 |
| 血统误判导致数据损坏 | 验证失败一律 RESYNC_REQUIRED；T7 测"不该 relink 时不 relink"；relink 前零本地写入 |
| 回滚丢弃 C 领先数据 | 正确语义（B 是新权威）但需文档/status 明示；`--check-peers` 让这条路尽量少走；回滚前要求本地无 watcher |
| ImageMeta JSON 演进 | 未知字段忽略；有界数组 |
| 与 #55803 类竞态（锚点在 relink 窗口内被 prune） | 验证在 `scan_remote` 持锁完成；复用现有 "remote start snapshot was deleted" 兜底；§3.6 豁免缩小窗口 |
| Δ 退化路径精度 | RFC 明示权衡；T14 验证正确性，性能作为可观测指标 |

## 8. 与后续 Phase 的接口

- Phase 1（push 单 peer）：不依赖 Phase 0
- Phase 2（push fan-out）：hub 对每个目标做 §3.2 验证，`local/remote` 互换；lineage 在 hub 本地可读；
  hub 天然看到所有备端 non-primary 快照，聚合视图在 hub 自然形成

## 9. 本机环境（WSL2，16 线程 / 宿主 16G / 946G 盘）

- 源码为 sparse checkout（25MB），需 `git sparse-checkout disable && git submodule update --init --recursive`
- 宿主建 `C:\Users\yzhuo\.wslconfig`：`memory=12GB swap=8GB processors=16`，然后 `wsl --shutdown`
- 工具链 gcc 15.2 / cmake 4.2.3 / Ubuntu 26.04 兼容性待验；失败则用 `ContainerBuild.md` 的容器构建
- 构建：`./install-deps.sh && ./do_cmake.sh -DWITH_TESTS=ON -DWITH_RADOSGW=OFF
  -DWITH_MGR_DASHBOARD_FRONTEND=OFF -DWITH_CEPHFS=OFF -DWITH_RBD_RWL=OFF`，
  `ninja -j4 ceph-mon ceph-osd ceph-mgr ceph rbd rados rbd-mirror ceph-authtool ceph-conf
  monmaptool cls_rbd unittest_rbd_mirror ceph_test_cls_rbd`
- 3 集群：`MON=1 OSD=1 MDS=0 RGW=0 ../src/mstart.sh clusterN -n --without-dashboard`，
  pool `size=1`；逻辑测试全覆盖，性能只取相对值（sync_bytes 全量 vs 增量）
- 可在本机完成：单测、mock、T1-T16 qa 逻辑；不可：teuthology suite（由上游 sepia 跑）
- 已验证：gcc 15.2 / cmake 4.2.3 / Ubuntu 26.04 全部兼容，`WITH_SYSTEM_BOOST=OFF`（系统无 1.87+）；
  编译 ~95 min；3 集群运行态 ~2.6 GB
- 实验脚本：`~/mirror-lab/mirror3.sh`（up/test/status/down）、`t1_baseline.sh`、`t2_force.sh`（MODE=equal|c_behind|c_ahead）
