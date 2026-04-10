//===-- shared_arena.h ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// 每核心共享预映射内存池 (Per-Core Shared Arena)
//
// 子课题一基础组件：面向跨进程大块内存委托的共享内存池。
//
// 设计要点（参见 DESIGN.md §2.1 - §2.4）：
//  - 物理内存池化：每 CPU 核心一个共享 Arena，使用 POSIX 共享内存 (shm_open)
//    作为物理页载体，所有参与进程映射同一 fd。
//  - 统一虚拟地址预留：所有进程将各核 Arena 映射到相同的固定虚拟地址区间，
//    因此池中块的 VA 在任何进程中都有效，实现零拷贝委托。
//  - 缓存交接：Secondary 释放大块内存时直接 store() 进 Arena 空闲列表；
//    后续 allocate() 先 retrieve() Arena，命中则无需 mmap 系统调用。
//  - rseq 快速 CPU ID：通过 rseq (Restartable Sequences) 零系统调用获取
//    当前核心 ID，比 sched_getcpu() 快一个数量级（§2.2）。
//  - 跨核迁移安全：释放时块存入其 VA 归属的 Arena（§2.4），跨核并发
//    访问通过 per-Arena futex 锁保护。
//
//===----------------------------------------------------------------------===//

#ifndef SCUDO_SHARED_ARENA_H_
#define SCUDO_SHARED_ARENA_H_

#include "atomic_helpers.h"
#include "common.h"
#include "internal_defs.h"
#include "platform.h"
#include "thread_annotations.h"


#if SCUDO_LINUX

namespace scudo {

// ---------------------------------------------------------------------------
// 全局常量
// ---------------------------------------------------------------------------

// 所有进程中 Arena 起始映射的固定虚拟地址。
// Linux 选取 32TB 附近的空洞区域，与典型 ASLR 布局冲突概率低。
// Android 真机常见 39-bit 用户态 VA（512 GB），因此改用更低地址窗口。
// 最终产品中可通过配置、broker 发布或内核保留区域机制确定。
#if SCUDO_ANDROID
static constexpr uptr kSharedArenaBaseAddr = 0x1000000000ULL; // 64 GB
#else
static constexpr uptr kSharedArenaBaseAddr = 0x200000000000ULL; // 32 TB
#endif

// 每个核心 Arena 的容量（256 MB）。
static constexpr uptr kArenaCapacityPerCore = 256ULL * 1024 * 1024;

// 支持的最大 CPU 核心数。
static constexpr u32 kArenaMaxCores = 16;

// Arena 头部元数据区域大小（一页，4 KB）。
// SharedArenaHeader 必须放入此范围内。
static constexpr uptr kArenaHeaderSize = 4096;

// 侵入式空闲链表的结束哨兵。
// 页偏移 0 是合法的数据区首页（DataBase 已跳过 header），
// 因此使用 UINT32_MAX（Arena 最大 65536 页，远不可能达到）。
static constexpr u32 kFreeListEnd = 0xFFFFFFFFU;

// FreeBlockHeader 校验魔数，用于区分有效空闲块与未初始化内存。
static constexpr u32 kFreeBlockMagic = 0xF4EEB10Cu;

// ---------------------------------------------------------------------------
// 内存压力水位线（DESIGN.md §2.1）
// ---------------------------------------------------------------------------

static constexpr u32 kMemoryPressureThresholdPercent = 80;
static constexpr u32 kPressureCheckInterval = 64;

// ---------------------------------------------------------------------------
// FreeBlockHeader：侵入式空闲块头部（DESIGN.md §2.2）
//
// 写在每个空闲块的起始地址处。被释放的块已拥有物理页，写入不会触发额外
// page fault。使用相对于 Arena 数据区起始（DataBase = BaseAddr +
// kArenaHeaderSize）的页偏移，而非绝对 VA，节省空间并天然兼容 32/64 位。
// ---------------------------------------------------------------------------
struct FreeBlockHeader {
  u32 PrevPageOff;   // 前驱空闲块的页偏移（kFreeListEnd = 链表头）
  u32 NextPageOff;   // 后继空闲块的页偏移（kFreeListEnd = 链表尾）
  u32 SizeInPages;   // 本空闲块的页数
  u32 Magic;         // == kFreeBlockMagic 时为有效空闲块
};

// ---------------------------------------------------------------------------
// SharedArenaHeader：每个 Arena 共享内存前 kArenaHeaderSize 字节内的元数据
//
// 该结构体存放于共享内存，所有映射该 Arena 的进程均可见并可修改。
// 跨进程锁通过 futex 实现（Lock 字段直接用于 FUTEX_WAIT/WAKE）。
//
// 内存管理采用 Bump 指针 + 侵入式空闲链表混合方案（DESIGN.md §2.2）：
//  - BumpOffsetInPages：单调递增，首次分配时推进，页面通过 page fault
//    按需提交，零系统调用、零元数据写入。
//  - FreeListHeadPageOff：VA 有序的双向侵入式空闲链表头，释放后的块
//    按地址插入，支持前后合并，无容量上限。
// ---------------------------------------------------------------------------
struct alignas(64) SharedArenaHeader {
  atomic_u32 Lock;
  atomic_u32 Version;

  // Bump 指针：当前已推进到的页偏移（相对 DataBase，单位：页）。
  // 初始值 0，表示整个数据区尚未分配。
  u32 BumpOffsetInPages;

  // 数据区总页数（= (kArenaCapacityPerCore - kArenaHeaderSize) / PageSize）。
  u32 TotalDataPages;

  // 侵入式空闲链表头（页偏移），kFreeListEnd = 空。
  u32 FreeListHeadPageOff;

  // 当前空闲链表中的块数量。
  u32 FreeCount;

  // 统计信息（best effort，无需强一致）。
  uptr TotalDonatedBytes;
  uptr TotalRetrievedBytes;
  u32  DonateCount;
  u32  RetrieveCount;
};

static_assert(sizeof(SharedArenaHeader) <= kArenaHeaderSize,
              "SharedArenaHeader 超过了 kArenaHeaderSize，请扩大头部区域");

// ---------------------------------------------------------------------------
// SharedArena：单个核心的共享 Arena
// ---------------------------------------------------------------------------
class SharedArena {
public:
  // 初始化 CoreId 号 Arena。
  // Linux 使用（或重新打开）POSIX 共享内存 "/scudo_arena_N"；
  // Android 使用 memfd / ASharedMemory，并通过继承 fd 的方式 attach。
  // 两平台都会将共享内存映射至固定 VA：
  // kSharedArenaBaseAddr + CoreId * kArenaCapacityPerCore。
  bool init(u32 CoreId);

  // 将一个块归还到 Arena（DESIGN.md §2.1 缓存交接 + §2.4 跨核迁移）。
  // CommitBase / CommitSize 均为绝对 VA，位于本 Arena 数据区范围内。
  // 按 VA 地址有序插入侵入式双向空闲链表，并自动合并相邻空闲块。
  // 永远成功（侵入式链表无容量上限），因此返回 void。
  void store(uptr CommitBase, uptr CommitSize);

  // 尝试分配一个满足 [Size + HeadersSize, Alignment] 的块。
  // 优先从侵入式空闲链表做 best-fit 查找（+ 尾部切割），
  // 未命中则从 Bump 指针分配（页面通过 page fault 按需提交）。
  // 成功时填写 OutCommitBase / OutCommitSize / OutEntryHeaderPos，
  // 三者均为绝对 VA，可在任何映射了同一 Arena 的进程中直接使用。
  bool retrieve(uptr Size, uptr Alignment, uptr HeadersSize,
                uptr &OutCommitBase, uptr &OutCommitSize,
                uptr &OutEntryHeaderPos);

  // 该 Arena 是否已成功初始化。
  bool isInitialized() const { return Initialized; }

  // 该 Arena 的固定 VA 起始地址。
  uptr getBaseAddr() const { return BaseAddr; }

  // Arena 对应的 CPU/Core 编号（调试输出用）。
  u32 getCoreId() const { return CoreId; }

  // 判断绝对 VA 是否落在本 Arena 的映射范围内。
  bool containsVa(uptr Va) const {
    return Initialized && Va >= BaseAddr &&
           Va < BaseAddr + kArenaCapacityPerCore;
  }

  // 将 [Addr, Addr+Size) 范围内的物理页归还给内核（DESIGN.md §2.1）。
  // 底层调用 madvise(MADV_REMOVE) 在 tmpfs 上打孔，实际释放物理内存。
  // 用于 Arena 总量超限时强制释放。
  bool releasePages(uptr Addr, uptr Size);

  // 统计信息（调试/测试用）。
  void getStats(uptr &OutFreeCount, uptr &OutDonatedBytes,
                uptr &OutRetrievedBytes) const;

  // 清空该 Arena 的共享状态，保留映射与 backing store 本身。
  // 用于 benchmark 在同一父进程下多轮独立进程对比时重新开始。
  void reset();

  int getShmFd() const { return ShmFd; }

private:
  // 跨进程 futex 自旋锁（存储于共享内存中的 Hdr->Lock）。
  void lock();
  void unlock();

  // 页偏移 ↔ 绝对 VA 转换
  uptr pageOffToAddr(u32 PageOff) const {
    return DataBase + static_cast<uptr>(PageOff) * getPageSizeCached();
  }
  u32 addrToPageOff(uptr Addr) const {
    return static_cast<u32>((Addr - DataBase) / getPageSizeCached());
  }

  // 读取空闲块头部
  FreeBlockHeader *getFreeBlock(u32 PageOff) const {
    return reinterpret_cast<FreeBlockHeader *>(pageOffToAddr(PageOff));
  }

  bool              Initialized = false;
  u32               CoreId      = 0;
  int               ShmFd       = -1;
  uptr              BaseAddr    = 0;      // 固定 VA 起始
  uptr              DataBase    = 0;      // 数据区起始 = BaseAddr + kArenaHeaderSize
  SharedArenaHeader *Hdr        = nullptr; // 指向映射后的元数据头
};

// ---------------------------------------------------------------------------
// SharedArenaPool：全局单例，管理最多 kArenaMaxCores 个 Arena（每核一个）
// ---------------------------------------------------------------------------
class SharedArenaPool {
public:
  static SharedArenaPool &getInstance();

  // Allow constructing the global singleton instance in shared_arena_linux.cpp.
  // External code should still prefer getInstance().
  SharedArenaPool() = default;

  // 初始化所有核心的 Arena（幂等，多次调用安全）。
  // 应在分配器首次使用前调用（例如在 Allocator::init() 中）。
  void init();

  // 重置所有已初始化 Arena 的共享状态，保留已有映射和 backing fds。
  void reset();

  // 共享内存池是否就绪。
  bool isReady() const { return Initialized; }

  // 获取指定核心的 Arena；CoreId 越界则返回 nullptr。
  SharedArena *getArena(u32 CoreId) {
    if (CoreId >= NumCores)
      return nullptr;
    return &Arenas[CoreId];
  }

  // 获取当前 CPU 核心对应的 Arena。
  // 优先通过 rseq 零系统调用获取 CPU ID（DESIGN.md §2.2），
  // rseq 不可用时回退到 sched_getcpu()。
  SharedArena *getCurrentArena();

  // 根据绝对 VA 找到其归属的 Arena（DESIGN.md §2.4 跨核迁移释放路径）。
  // VA 必须落在某个 Arena 的映射范围内，否则返回 nullptr。
  SharedArena *getOwningArena(uptr Va);

  // 是否应当使用 Arena 进行分配（DESIGN.md §2.1 水位线机制）。
  // 周期性读取 /proc/meminfo 检测内存压力，超过阈值返回 true。
  bool shouldUseArena();

  u32 getNumCores() const { return NumCores; }

  // 判断绝对 VA 是否属于任意 Arena（用于 deallocate 路由判断）。
  bool isArenaAddr(uptr Va) const {
    if (!Initialized)
      return false;
    return Va >= kSharedArenaBaseAddr &&
           Va < kSharedArenaBaseAddr +
                    static_cast<uptr>(NumCores) * kArenaCapacityPerCore;
  }

private:
  // 检测当前系统内存压力是否超过水位线（读取 /proc/meminfo）。
  bool checkMemoryPressure();

  bool        Initialized  = false;
  u32         NumCores     = 0;
  SharedArena Arenas[kArenaMaxCores];

  // 内存压力水位线状态（DESIGN.md §2.1）。
  // ArenaActive 由 shouldUseArena() 周期性更新，多线程读取可能存在短暂
  // 不一致，但作为启发式开关这是可接受的。
  atomic_u32  AllocCounter;
  bool        ArenaActive  = false;
};

// 调试/测试辅助：
//  - SCUDO_SHARED_ARENA_FORCE=1: 测试时强制走共享 Arena 路径。
//  - SCUDO_SHARED_ARENA_TRACE=1: 输出共享 Arena 关键路径日志。
//  - SCUDO_SHARED_ARENA_ATTACH=1:
//      Linux: attach 到现有具名 shm。
//      Android: attach 到父进程继承下来的 backing fd。
bool sharedArenaForceEnabled();
void setSharedArenaForceForTesting(bool Enabled);
void clearSharedArenaForceForTesting();
bool sharedArenaTraceEnabled();
void sharedArenaTrace(const char *Format, ...);

} // namespace scudo

#endif // SCUDO_LINUX

#endif // SCUDO_SHARED_ARENA_H_
