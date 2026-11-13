# RBD push 架构快照异步复制 — 实现分析

> 基于 ceph main 分支 (commit 2d0cd8a, 2026-09-26) 源码。
> 结论先行：rbd-mirror 当前**只支持 pull**（daemon 跑在备端拉数据），
> 但代码里已经为 push 预留了地基（peer direction / 快照标签），
> 实现 push 的本质是把 daemon 里 "local=副本端、remote=源端" 的固化假设反转。

---

## 1. 现有 pull 架构（先理解它才能改）

### 1.1 组件调用链

```
rbd-mirror daemon (部署在【副本端】集群)
└─ Mirror (src/tools/rbd_mirror/Mirror.cc)
   └─ ClusterWatcher::read_pool_peers          ← 扫本地池 mirror peer，
   │     只保留 direction != TX 的 peer            (ClusterWatcher.cc:130 行附近)
   └─ PoolReplayer (每个 本地pool+peer 一个)     ← PoolReplayer.cc
      │   · init_rados: 连本地(自身) + 连远端(peer 凭据)
      │   · RemotePoolPoller: 拿远端 mirror_uuid、
      │     mirror_peer_ping() 在远端注册自己为 tx-only peer、
      │     mirror_peer_list 找到自己站的 peer uuid → RemotePoolMeta
      │   · LeaderWatcher: 本地池 leader 对象选举（多 daemon HA 分片）
      └─ NamespaceReplayer (每个本地 namespace 一个)   ← NamespaceReplayer.cc
          · PoolWatcher ×2: watch 本地+远端的 rbd_mirroring 对象
            (refresh = cls mirror_image_list 列出 global_image_id)
          · ImageMap (leader): global_image_id → daemon instance 分片
          · InstanceWatcher/InstanceReplayer: 按分片创建 ImageReplayer
          · MirrorStatusUpdater ×2: 状态写本地池 + 本站状态写远端池
          · ImageDeleter: 本地 trash 清理
          └─ ImageReplayer (每个 global_image_id 一个)
             └─ BootstrapRequest               ← 建立 local/remote ImageCtx
             │   prepare_local_image  → 本地镜像必须非 primary
             │   prepare_remote_image → 远端必须 primary（或待建）
             │   open_remote_image / create_local_image / open_local_image
             │   prepare_replay (journal/snapshot 由 StateBuilder 分派)
             └─ image_replayer::snapshot::Replayer (pull 数据通路核心)
                 scan_local_mirror_snapshots  → 本地最近 non-primary 快照(续传点)
                 scan_remote_mirror_snapshots → 远端 primary 快照，
                     挑 mirror_peer_uuids ∋ 我方在远端的 peer uuid
                 copy_snapshots   → SnapshotCopyRequest remote→local (快照元数据)
                 get_remote_image_state → GetImageStateRequest (ImageMeta)
                 create_non_primary_snapshot → CreateNonPrimaryRequest(本地)
                 update_mirror_image_state → 本地 mirror_image CREATING→ENABLED
                 request_sync → InstanceWatcher 节流
                 copy_image → ImageCopyRequest remote→local (数据 delta)
                 apply_image_state → ApplyImageStateRequest
                 update_non_primary_snapshot → cls 写进度到本地 header
                 notify_image_update → unlink_peer (远端快照摘掉我方 peer uuid)
                 → 回到扫描循环；空闲时靠远端 image update watcher 唤醒
```

### 1.2 关键数据结构（cls_rbd_types.h / librbd.h）

| 结构 | 作用 | push 相关字段 |
|---|---|---|
| `MirrorPeer` | 池级 peer 记录 | `mirror_peer_direction` ∈ {RX, TX, RX_TX}，`client_name` 仅 RX 类需要 |
| `MirrorImage` | rbd_mirroring 目录项 | `mode`=JOURNAL/SNAPSHOT, `state`=CREATING/ENABLED/… |
| `MirrorSnapshotNamespace` | 镜像快照命名空间 | `state`(PRIMARY/PRIMARY_DEMOTED/NON_PRIMARY/NON_PRIMARY_DEMOTED)，`primary_mirror_uuid`，`primary_snap_id`，**`mirror_peer_uuids`（还欠谁的快照）**，`complete`，`snap_seqs`（远端snap→本地snap 映射）, `clean_since_snap_id` |
| `MirrorImageSiteStatus` | 每站点状态 | 按 mirror_uuid 区分站点，写进各池 rbd_mirroring |
| `ImageMeta` (mirror/snapshot) | 随快照走的镜像元数据 | 特性/父链等，apply 到副本端 |

**重要**：primary 快照打标签时已考虑 TX peer ——
`CreatePrimaryRequest.cc:97` 跳过 `RX` peer，把 TX/RX_TX peer uuid 写进
`mirror_peer_uuids`；没有 tx 类 peer 直接报 `"no mirror tx peers configured"`。
所以 push 模式下"快照该推给谁"的元数据**已经存在**，无需改快照创建路径。

### 1.3 凭据/拓扑现状

- `bootstrap create`（在集群 X）：产出含 X 的 fsid/mon_host/`rbd-mirror-peer` 客户端 key 的 token
- `bootstrap import`（在集群 Y）：校验 direction ∈ {rx-only, rx-tx}（`api/Mirror.cc:1431` 拒绝 TX），
  在 Y 建 peer 记录并把 X 的 mon_host/key 存进 Y 的 config-key
  `rbd-mirror/peer/<pool_id>/<uuid>`（`resolve_peer_site_config_keys` 读取）
- `mirror_peer_ping`（cls）：由拉端 daemon 周期性调用，在【源端】池上把
  自己注册成 `TX` peer（cls_rbd.cc:4844）——即"源端知道自己要向谁吐数据"
- `peer_add`（cls_rbd.cc:4895）**拒绝 direction=TX** —— TX peer 只能由 ping 自动产生，
  因此**目前没有任何办法在源端创建一个带远端凭据的 TX peer**（push 的控制面缺口）

---

## 2. Push 改动地图

按改动层次排列，标注文件与大致位置。

### 2.1 控制面：peer 方向 + 凭据（必改）

| 位置 | 现状 | push 需要 |
|---|---|---|
| `src/librbd/api/Mirror.cc` `peer_bootstrap_import()` ~L1431 | direction 只接受 RX/RX_TX | 允许 `TX`（或新增 tx-only import）：在推送端本地创建 TX peer 记录，并用 `create_bootstrap_peer()` 把远端 mon_host/key 写进本地 config-key |
| `src/cls/rbd/cls_rbd.cc` `mirror::peer_add()` ~L4895 | 拒绝 TX | 放行 bootstrap/管理路径创建 TX peer；或新增 `peer_ping` 变体携带凭据。最小侵入做法：加一个带 creds 的新 cls op，不动现有 `peer_add` 校验 |
| `src/tools/rbd/action/MirrorPool.cc` `add_direction_optional()` ~L87 | `--direction` 只认 rx-only/rx-tx | 增加 `tx-only`；`execute_peer_set` 已有 direction 路径（`peer_site_set_direction`） |
| `src/tools/rbd_mirror/Types.h` `PeerSpec` | 无 direction 字段 | 增加 direction，ClusterWatcher 填充 |
| `src/tools/rbd_mirror/ClusterWatcher.cc` `read_pool_peers()` | `peer.direction != TX` 才入列 | TX peer 也入列（带 direction 标记），`resolve_peer_site_config_keys` 读凭据逻辑已通用，无需改 |

PoC 捷径（不改 bootstrap/cls/api）：`peer set` 命令已放行 tx-only
（`MirrorPool.cc` validate 的 `permit_tx` 在 peer set 路径开启），且
`peer set ... mon-host`/`key-file` 能把凭据写进 config-key ——
即今天就能手工造出"带凭据的 TX peer"：

```bash
rbd mirror pool peer add pool client.mirror@site-b      # 先建 rx-tx
rbd mirror pool peer set pool <uuid> mon-host <B mons>  # 写远端 mon
rbd mirror pool peer set pool <uuid> key-file <B key>   # 写远端 key
rbd mirror pool peer set pool <uuid> direction tx-only  # 翻成 tx-only
```

上游化时再把 `peer_bootstrap_import` 的 TX 路径和 cls `peer_add` 限制补齐。

### 2.2 daemon 框架（必改）

| 位置 | 现状 | push 需要 |
|---|---|---|
| `Mirror.cc` `update_pool_replayers()` | peer → PoolReplayer 一一对应 | peer 带 direction；对 TX(-capable) peer 建 push 型 replayer。RX_TX peer 可能同时 pull+push —— 设计决策点（建议先只做纯 TX 单 push） |
| `PoolReplayer.cc` `init()` | remote=peer(源), local=本地(副本) | 语义变为 local=源、remote=副本。`init_rados`、RemotePoolPoller、LeaderWatcher 本身方向无关，可复用；但要传一个 `is_push` 标志下去 |
| `RemotePoolPoller.cc` | 对远端 ping 自己 = 在远端注册"我是消费者"(TX) | push 语义相反：本端是生产者。需要①远端对本端有一条 RX 记录（可手工建），②拿远端 pool 的 mirror_uuid 仍然需要。可以改造 poller 或在 push 下简化 |
| `NamespaceReplayer.cc` | local/remote PoolWatcher、双 status updater、ImageMap、InstanceWatcher | 大体不变。`m_local_pool_watcher` 在 push 下正好给出"本地 primary 镜像集合"（推送候选）；`m_remote_pool_watcher` 给远端已建副本，用于补建/删除检测 |
| `ImageDeleter.cc` | 本地发现"远端主镜像没了"→ 本地入 trash | push 场景：本地 primary 删除/取消后远端副本清理 —— 需要 push 语义的新逻辑（或 PoC 阶段先不管删除传播） |

### 2.3 每镜像 Bootstrap（必改，`image_replayer/BootstrapRequest.cc`）

现在版本：本地是副本（非 primary 才继续，否则 ENOMSG），远端是源。
push 版需要一个平行的判定路径（建议 `BootstrapRequest` 加 `bool push_mode` 或
拆 `PushBootstrapRequest`）：

- `prepare_local_image`：改为要求**本地是 primary**（有 primary mirror 快照），
  `is_local_primary()` 为真继续而非跳过（现在 L176 直接 finish(-ENOMSG)）
- `prepare_remote_image`：远端应 non-primary 或不存在；
  不存在 → 新增 `CreateRemoteImageRequest`：
  参考 `snapshot/CreateLocalImageRequest.cc`（在【远端】rbd_mirroring 写
  `mirror_image CREATING` → `CreateImageRequest`（ioctx=远端、src=本地 ctx）
  → 首个 `CreateNonPrimaryRequest`）。现有类基本都是 ioctx 无关的，参数对调即可
- `open_local_image`（源端）建议 `read_only=true`；`open_remote_image`（副本端）可写
  —— `OpenImageRequest`/`OpenLocalImageRequest` 已有 read_only 参数
- `PrepareReplayRequest`（snapshot 版）：判定逻辑要 push 化——
  检查远端镜像 non-primary、是否 linked、resync 标记（resync 标记在远端镜像上）

### 2.4 核心：`image_replayer/snapshot/Replayer.cc`（最大改动点，约 1700 行）

pull 与 push 的状态机完全对称，把 local↔remote 角色互换即可。
推荐做法：**参数化而不是复制**——把类里的 `local_image_ctx`/`remote_image_ctx`
抽象为 `src_image_ctx`(primary) / `dst_image_ctx`(non-primary)，
外层（StateBuilder）按 push/pull 决定谁是谁；或者新写 `PushReplayer` 复用内部
helper。逐函数对应关系：

| pull 现状（函数） | push 需要 |
|---|---|
| `scan_local_mirror_snapshots()` 找本地最后 non-primary 快照做续传锚点 | 改成 `scan_remote` 上找 non-primary 快照（primary_mirror_uuid==本地uuid, primary_snap_id==本地snap） |
| `scan_remote_mirror_snapshots()` 扫远端 primary 快照，`mirror_peer_uuids ∋ remote_mirror_peer_uuid` 过滤 | 扫**本地** primary 快照，`mirror_peer_uuids ∋ 本地池里远端站的 peer uuid`（= tx peer 的 PeerSpec.uuid）；demote/split-brain 判定镜像对称反转 |
| `copy_snapshots()` `SnapshotCopyRequest(remote→local)` | `SnapshotCopyRequest(local→remote)`（类本身是 src/dst 参数化，直接用） |
| `get_remote_image_state()` `GetImageStateRequest(remote)` | `GetImageStateRequest(local)`（源端末位快照取 ImageMeta） |
| `create_non_primary_snapshot()` 在本地建 non-primary | 在**远端**建 non-primary：`primary_mirror_uuid`=本地池 mirror_uuid，`primary_snap_id`=本地 end snap |
| `update_mirror_image_state()` 本地 CREATING→ENABLED | 远端 rbd_mirroring 上的镜像条目标 ENABLED（`ImageStateUpdateRequest` ioctx 换远端） |
| `request_sync()`/`copy_image()` `ImageCopyRequest(remote→local)` + `last_copied_object_number` 断点续传 | `ImageCopyRequest(local→remote)`；进度记在远端 non-primary 快照命名空间（`last_copied_object_number` 同样在 MirrorSnapshotNamespace 里） |
| `apply_image_state()` 远端 ImageMeta 应用到本地 | 应用到远端（`ApplyImageStateRequest` 参数换序） |
| `update_non_primary_snapshot()` cls 写本地 header | cls 写远端 header |
| `notify_image_update()` 本地镜像广播 | 远端镜像广播（唤醒远端侧任何依赖方，可选） |
| `unlink_peer(remote_snap_id)` 在远端摘 `m_remote_mirror_peer_uuid` | 在**本地**源端快照摘 tx peer uuid（`UnlinkPeerRequest` on local）→ 让 rbd_support 能回收快照 |
| `register_remote_update_watcher()` watch 远端 image header 等新快照 | watch **本地** image（rbd_support 打 mirror 快照即唤醒推送）——push 下反而是本地 watch，更简单 |
| `is_remote_primary()` / demote / split-brain 检测 | 全部对称反转；远端被 promote 后推送方应停止并上报告警 |

其他小点：
- perf counters（`l_rbd_mirror_snapshot_*`）通用，复用
- `PoolMetaCache`：local/remote pool meta 缓存，推送下仍需要远端 pool mirror_uuid（写进 non-primary snap 的 primary_mirror_uuid）
- `StateBuilder.h` 里 `local_primary_mirror_uuid`/`is_local_primary`/`is_remote_primary`/`remote_mirror_peer_uuid` 的含义要写清楚（建议改名成 source/dest 语义）

### 2.5 状态上报（改动小）

- `MirrorStatusUpdater` 双实例结构不变：把"本地站点状态"写到副本所在池的
  rbd_mirroring（push 下=远端池），语义由 push/pull 决定写到哪个 ioctx。
- `mirror_image_status_get_summary`（cls_rbd.cc:6528）已经按 peer direction
  过滤统计 tx peer 的状态 —— 上游早已预留。
- `rbd mirror image status` / pool status 的展示语义需要按 push 场景核对一遍。

### 2.6 不需要改的部分

- **快照调度**：`rbd_support` mgr 模块 + `rbd mirror snapshot schedule` 本来就在
  源端创建 primary 快照，零改动
- **CreatePrimaryRequest**：已给 TX peer 打 `mirror_peer_uuids` 标签，零改动
- **deep_copy 全套**：`ImageCopyRequest`/`SnapshotCopyRequest`/`ObjectCopyRequest`
  方向无关，零改动
- **LeaderWatcher/InstanceWatcher/ImageMap**：分片逻辑只看 global id，零改动
- **cephx/连接**：`init_rados` + config-key 凭据解析已通用

### 2.7 明确不在范围内

- journal 模式 push（journal client 注册/tag 机制复杂得多，建议 push 只做 snapshot 模式）
- group/一致性组镜像（上游还在开发中）
- 跨 pool 不同名映射（现有约束保留）

---

## 3. 需要学习的代码知识（按依赖顺序）

| 优先级 | 内容 | 位置 |
|---|---|---|
| ★★★ | mirror 元数据模型：MirrorPeer/MirrorImage/MirrorSnapshotNamespace/镜像状态机语义 | `src/cls/rbd/cls_rbd_types.h`、`src/cls/rbd/cls_rbd.cc`（peer_ping/peer_add/status 方法）、`doc/rbd/rbd-mirroring.rst` |
| ★★★ | snapshot::Replayer 完整状态机（pull 数据通路） | `src/tools/rbd_mirror/image_replayer/snapshot/Replayer.cc` (~1700 行) |
| ★★★ | librbd deep_copy 三件套 | `src/librbd/deep_copy/{ImageCopyRequest,SnapshotCopyRequest,ObjectCopyRequest}.cc` |
| ★★★ | librbd mirror 快照请求集 | `src/librbd/mirror/snapshot/{CreatePrimaryRequest,CreateNonPrimaryRequest,DemoteRequest,UnlinkPeerRequest,GetImageStateRequest,ImageMeta,Types}.cc` |
| ★★☆ | daemon 骨架分层 | `Mirror.cc → ClusterWatcher.cc → PoolReplayer.cc → RemotePoolPoller.cc → NamespaceReplayer.cc → InstanceWatcher.cc + ImageMap.cc + InstanceReplayer.cc → ImageReplayer.cc → BootstrapRequest.cc` |
| ★★☆ | BootstrapRequest + PrepareLocal/RemoteImageRequest + StateBuilder（模式分派点） | `src/tools/rbd_mirror/image_replayer/` |
| ★★☆ | 镜像目录监听机制 | `src/librbd/MirroringWatcher.cc`（rbd_mirroring notify）、`PoolWatcher.cc`、`pool_watcher/RefreshImagesRequest.cc` |
| ★★☆ | cephfs-mirror 作为 push 架构的现成参照 | `src/tools/cephfs_mirror/{Mirror,PeerReplayer,ClusterWatcher}.cc`、`doc/dev/cephfs-mirroring.rst` |
| ★☆☆ | async 编程套路 | `BaseRequest`/`ContextWQ`/`create_rados_callback`/`create_context_callback`/`C_SaferCond`/`C_Gather` |
| ★☆☆ | service daemon/asok 上报 | `ServiceDaemon.cc`、`MirrorStatusUpdater.cc` |
| ★☆☆ | bootstrap/凭据 | `librbd/api/Mirror.cc`（peer_bootstrap_*/create_bootstrap_peer/peer_site_*）、`tools/rbd/action/MirrorPool.cc` |

建议的阅读顺序：cls_rbd_types.h → doc/rbd/rbd-mirroring.rst →
snapshot/Replayer.cc 全文 → BootstrapRequest.cc → NamespaceReplayer/PoolReplayer →
librbd deep_copy 与 mirror/snapshot 请求 → cephfs_mirror/PeerReplayer.cc 对照。

---

## 4. PoC 验证方案（三层递进）

### 阶段 A：零代码——先验证数据通路语义（半天）

用两个 `mstart.sh` 本地集群手工走一遍"推"的数据面：

```bash
# src/mstart.sh <name> -n   (-n = 全新集群)
../src/mstart.sh cluster1 -n --without-dashboard
../src/mstart.sh cluster2 -n --without-dashboard

# cluster1 建镜像 + 快照
rbd --cluster cluster1 create pool1/img --size 128
rbd --cluster cluster1 bench --io-type write img ...  # 写数据
rbd --cluster cluster1 snap create pool1/img@s1
rbd --cluster cluster1 bench ...                       # 再写
rbd --cluster cluster1 snap create pool1/img@s2

# cluster2 建同规格副本镜像后，手工推送 delta
rbd --cluster cluster1 export-diff pool1/img@s1 - | \
    rbd --cluster cluster2 import - pool1/img
rbd --cluster cluster1 export-diff --from-snap s1 pool1/img@s2 - | \
    rbd --cluster cluster2 import-diff - pool1/img
rbd --cluster cluster2 snap create pool1/img@s1 ; rbd --cluster cluster2 snap create pool1/img@s2
```

目的：确认跨集群 diff 语义、`fast-diff` 依赖、`export-diff|import-diff` 管道带宽。
注意：import-diff 不产生 mirror 命名空间快照，failover 语义不成立——只验证数据面。

### 阶段 B：最小 daemon 改造 PoC（核心工作量）

目标：改完的 `rbd-mirror` 跑在**源端集群**，把镜像快照 delta 推给副本端。

最小改动集（快速 PoC，非上游质量）：
1. `PeerSpec` 加 direction；`ClusterWatcher::read_pool_peers` 放行 TX 方向 peer
2. `PoolReplayer` 透传 `push_mode`（peer.direction==TX）
3. `BootstrapRequest` 加 push 分支：`is_local_primary()` 继续；远端缺镜像→
   `CreateRemoteImageRequest`（镜像 `snapshot/CreateLocalImageRequest` 改 ioctx 实现）
4. `snapshot::PushReplayer`：先直接复制 `Replayer.cc` 改 local/remote 分工
   （上游质量的实现应抽象 src/dst，但 PoC 复制文件最快）
5. peer 配置 PoC 阶段用手工 `peer add`（rx-tx 记录 + 远端 conf/keyring 文件），
   绕开 bootstrap 的 TX 限制

验证环境（复用 qa 基建）：
```bash
# qa/workunits/rbd/rbd_mirror_helpers.sh 的 setup() 就是这么干的：
# mstart.sh cluster1/cluster2 → 编译树内跑 rbd-mirror
RBD_MIRROR_MODE=snapshot qa/workunits/rbd/rbd_mirror.sh
# push PoC：把 start_mirror 指向 cluster1（源端）跑改造后的二进制
```

验证场景清单（按 rbd_mirror.sh 现有 case 对齐）：
- 基础推送：写→`rbd mirror image snapshot`→远端出现 non-primary 快照、数据一致（`compare_images`）
- 增量推送：第二次快照只传 delta（观察 perf counter `l_rbd_mirror_snapshot_sync_bytes`）
- 断点续传：推送中途 kill daemon，重启后从 `last_copied_object_number` 续推
- 清理：远端副本同步完成后本地 `mirror_peer_uuids` 被摘除（unlink_peer），快照可被 rbd_support 回收
- failover：远端 `promote` → 本地推送应停止并报错（防止脑裂写）
- 新镜像/删除/禁用镜像路径

### 阶段 C：工程化验证

- `src/test/rbd_mirror/` 单测框架（`test_mock_Replayer.cc` 等用 mock ImageCtx
  跑状态机，不用真集群）——给 PushReplayer 补对称 mock 测试
- qa workunit 加 `RBD_MIRROR_MODE=push` 或新 `rbd_mirror_push.sh`；
  teuthology `rbd:mirror` suite 接入
- 多 daemon HA（LeaderWatcher 分片）+ 多 peer
- promote/demote/resync/split-brain 全流程

### 构建注意

- ceph 编译很重：`./do_cmake.sh`（可加 `-DWITH_TESTS=OFF` 等裁剪）→
  `cd build && ninja rbd-mirror rbd ceph-osd ceph-mon ceph-mgr` 起步；
  build 目录轻松 20-30GB，建议 Linux 原生文件系统（不要在 /mnt/c 下构建）、
  配 ccache。WSL 注意内存/交换空间。
- `mstart.sh/vstart.sh` 直接用 build 树里的二进制跑 dev 集群，无需安装。

---

## 5. 设计决策点（动手前先想清楚）

1. **单 daemon 双向还是专 daemon 单推？** 一个 RX_TX peer 理论上同时 pull+push。
   建议 PoC：TX-only peer 才走 push 路径，RX/RX_TX 维持 pull —— 语义不重叠、风险最小。
2. **复制 PushReplayer 还是抽象 src/dst？** PoC 复制快；合上游必然要求抽象（~1700 行重复维护不现实）。
3. **删除传播**：纯 push 部署（对端无 daemon）下"源端删镜像→远端副本清理"没有现成机制，需要新设计（远端 mirror_image 目录 + 本端比对）。
4. **resync/demote 命令语义**：`rbd mirror image resync` 等命令假定副本端执行，push 下要在远端镜像上生效——命令目标集群语义要过一遍。
5. **与上游对齐**：peer direction（tx）和快照打标明显是给 push 预留的地基
   （`"no mirror tx peers configured"` 报错措辞、`// the restriction is for rx`
   注释、`peer set direction tx-only` 均证实）。**已核查（2026-09）：
   tracker / ceph-devel / GitHub 均无人在做 push rbd-mirror**；
   当前在途镜像工作是 `wip-rbd-cgsm-base` 上的一致性组快照镜像（pull 架构）；
   PR #70805 "Orbital Snapshots" 是 RBD→S3 归档，不重叠。建议在 dev@ceph.io
   发提案帖占坑并与 Ilya Dryomov（direction 作者）对齐设计。

## 附：相关上游资料

- `doc/dev/cephfs-mirroring.rst` — 现成 push 架构文档（cephfs 侧）
- ceph/ceph PR #54633 — cephfs snapdiff 增量同步实现参考
- tracker #41554 — RBD 快照镜像原始设计拆解（看子任务列表即可了解模块边界）
- ceph/ceph PR #43228 — direction 语义修复记录（tx/rx peer 含义最清晰的说明）
