# 时延对比测试要点（脚本编写参考）

**环境**：一加 15（Android），本地跑带 Scudo + SharedArena 的测试二进制（与 `DESIGN.md` §2.1–§2.2、当前 demo 一致）。

**不测**：跨核迁移 / 跨核释放（§2.4，未实现）。所有参与线程 **绑核**，减少乱迁移对「当前核 Arena」的干扰。

---

## 两条路径（脚本里用开关切换）

| 路径 | 做法 |
|------|------|
| 委托 | `setSharedArenaForceForTesting(true)`，大块走 Arena `retrieve` / 释放 `store` |
| 传统 | `clearSharedArenaForceForTesting()`，走 MapAllocatorCache + `mmap` 类行为 |

同一块大小、同一种负载下各跑一轮，对比耗时。

---

## 脚本里建议做的事

1. **启动检查**：`SharedArenaPool::isReady()`；打印 `getNumCores()`、当前进程/线程 affinity。
2. **绑核**：主线程 + 每个工作线程 `sched_setaffinity`（或 `pthread_setaffinity_np`）。多线程时尽量 **一线程一核**，核号落在 Arena 管理的 `0 .. getNumCores()-1` 内。大中小核混排时注意：同一组对比里固定策略（例如全绑大核），避免两次跑法不一致。
3. **计时**：`allocate` 起止、`deallocate` 起止、或整轮 `alloc+dealloc`；时钟用单调时钟（如 `clock_gettime(CLOCK_MONOTONIC)`）。先 **预热** 若干轮再统计。
4. **统计**：每种（路径 × 块大小 × 线程模型）跑够迭代次数，输出均值 / P50 / P95 / P99（按需）。块大小可扫：单页附近、64K–1M 间若干点。
5. **可选**：冷热分开——预热后再记「热」；冷启动可进程重启或新映射后前几轮单独记。
6. **可选跨进程**：父释放 → 子同尺寸 `allocate` 计时；父子各自绑核。不测跨核。
7. **断言（抽样即可）**：委托路径指针 `isArenaAddr`；传统路径不应落在 Arena VA（或仅打日志）。

---

## 代码对照（写脚本时翻源码）

- 开关：`setSharedArenaForceForTesting` / `clearSharedArenaForceForTesting`（`shared_arena_linux.cpp`）
- 行为参考：`tests/secondary_test.cpp` 里 `SharedArenaDelegationTest`、`pinProcessToArenaCpu()`

---

## 运行参数补充（bench）

`tests/shared_arena_latency_bench.cpp` 支持：
- `--mode thread|process|independent-process`：默认 `thread`。
  - `process`：表示 `fork(parent->child)` 的多进程对比。
  - `independent-process`：表示父进程 `fork+exec` 多个“全新进程”（donor/worker）分配释放，用于“独立启动”场景的委托传递时延对比。

`independent-process` 模式额外支持：
- `--processes P`：参与计时的 worker 进程数量（默认 4）。

基于“应用使用期”的负载模拟：
- `--hold-us U`：在每次 `allocate` 后触摸内存并等待 U 微秒，再 `deallocate`（默认 0）。
- `--touch-pages 0|1`：触摸每页一个字节以提交物理页并模拟应用访问（默认 1）。

## 跨核相关

`getOwningArena` 等跨核专项等实现后再加用例；本脚本阶段不写。
