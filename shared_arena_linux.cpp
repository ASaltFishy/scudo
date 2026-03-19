//===-- shared_arena_linux.cpp ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Linux 平台的共享内存池实现。
//
// 核心机制：
//  1. 使用 shm_open("/scudo_arena_N") 创建具名共享内存，任意进程均可按名打开。
//  2. 用 MAP_SHARED | MAP_FIXED_NOREPLACE 将共享内存映射到固定 VA，使所有进程
//     的同一 Arena 虚拟地址完全一致，实现池中块的零拷贝跨进程复用。
//  3. 跨进程锁通过 futex(FUTEX_WAIT/WAKE) 实现，锁字段 (Hdr->Lock) 直接驻留
//     在共享内存中，对所有进程可见。
//  4. 内存管理采用 Bump 指针 + 侵入式空闲链表混合策略（DESIGN.md §2.2）：
//     首次分配走 Bump 指针（页面通过 page fault 按需提交），释放后的块
//     按 VA 有序插入侵入式双向链表，支持前后合并和 best-fit 分配。
//  5. CPU ID 获取通过 rseq TLS 零系统调用实现（DESIGN.md §2.2），回退到
//     sched_getcpu()。
//
//===----------------------------------------------------------------------===//

#include "platform.h"

#if SCUDO_LINUX

#include "shared_arena.h"

#include "common.h"
#include "internal_defs.h"
#include "string_utils.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <linux/rseq.h>
#include <sched.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

// glibc 2.35+ 自动为每个线程注册 rseq，并导出以下符号。
// 使用 weak 引用，在老版本 glibc 上优雅回退到 sched_getcpu()。
extern "C" {
__attribute__((weak)) extern const ptrdiff_t __rseq_offset;
__attribute__((weak)) extern const unsigned int __rseq_size;
}

// MAP_FIXED_NOREPLACE 在 Linux 4.17 引入；老内核回退到 MAP_FIXED。
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

namespace scudo {

static bool sharedArenaEnvEnabled(const char *Name) {
  const char *Value = getenv(Name);
  return Value != nullptr && Value[0] != '\0' && Value[0] != '0';
}

bool sharedArenaForceEnabled() {
  static const bool Enabled = sharedArenaEnvEnabled("SCUDO_SHARED_ARENA_FORCE");
  return Enabled;
}

bool sharedArenaTraceEnabled() {
  static const bool Enabled = sharedArenaEnvEnabled("SCUDO_SHARED_ARENA_TRACE");
  return Enabled;
}

void sharedArenaTrace(const char *Format, ...) {
  if (!sharedArenaTraceEnabled())
    return;

  static atomic_u32 TraceCount = {};
  const u32 Count =
      atomic_fetch_add(&TraceCount, 1u, memory_order_relaxed);
  if (Count >= 64) {
    if (Count == 64) {
      static constexpr char LimitMsg[] =
          "[scudo][shared_arena] trace limit reached, suppressing further logs\n";
      write(STDERR_FILENO, LimitMsg, sizeof(LimitMsg) - 1);
    }
    return;
  }

  char Buffer[512];
  int PrefixLen =
      snprintf(Buffer, sizeof(Buffer), "[scudo][shared_arena] ");
  if (PrefixLen < 0 || static_cast<size_t>(PrefixLen) >= sizeof(Buffer))
    return;

  va_list Args;
  va_start(Args, Format);
  int BodyLen = vsnprintf(Buffer + PrefixLen, sizeof(Buffer) - PrefixLen,
                          Format, Args);
  va_end(Args);
  if (BodyLen < 0)
    return;

  size_t TotalLen =
      Min(sizeof(Buffer) - 1, static_cast<size_t>(PrefixLen + BodyLen));
  if (TotalLen == 0 || Buffer[TotalLen - 1] != '\n')
    Buffer[TotalLen++] = '\n';
  write(STDERR_FILENO, Buffer, TotalLen);
}

// ---------------------------------------------------------------------------
// SharedArena::lock / unlock（跨进程 futex 自旋锁）
// ---------------------------------------------------------------------------

void SharedArena::lock() {
  DCHECK(Hdr != nullptr);
  // 快速路径：尝试 CAS 0 → 1。
  // 使用返回 bool 的指针重载（overload 1）：
  //   成功：返回 true，退出循环；
  //   失败：Cmp 被更新为当前值（1），返回 false，进入 FUTEX_WAIT。
  u32 Expected = 0u;
  while (!atomic_compare_exchange_strong(&Hdr->Lock, &Expected, 1u,
                                         memory_order_acquire)) {
    // 慢路径：通过 FUTEX_WAIT 进入睡眠，避免忙等占用 CPU。
    syscall(SYS_futex,
            reinterpret_cast<uptr>(&Hdr->Lock.ValDoNotUse),
            FUTEX_WAIT, 1, nullptr, nullptr, 0);
    Expected = 0u; // 重置期望值，准备下次尝试
  }
}

void SharedArena::unlock() {
  DCHECK(Hdr != nullptr);
  atomic_store(&Hdr->Lock, 0u, memory_order_release);
  // 唤醒至多一个等待者。
  syscall(SYS_futex,
          reinterpret_cast<uptr>(&Hdr->Lock.ValDoNotUse),
          FUTEX_WAKE, 1, nullptr, nullptr, 0);
}

// ---------------------------------------------------------------------------
// SharedArena::releasePages — 释放 Arena 内指定范围的物理页（DESIGN.md §2.1）
//
// 使用 madvise(MADV_REMOVE) 在 tmpfs/shmem 上打孔，真正释放物理内存。
// 后续对该范围的读取将触发 page fault 并返回零页。
// 场景：Arena 元数据槽位满时，无法将块记入 freelist，改为直接释放物理页。
// ---------------------------------------------------------------------------

bool SharedArena::releasePages(uptr Addr, uptr Size) {
  if (UNLIKELY(!Initialized || Size == 0))
    return false;

  const uptr PageSize = getPageSizeCached();
  const uptr AlignedAddr = roundUp(Addr, PageSize);
  const uptr AlignedEnd = roundDown(Addr + Size, PageSize);
  if (AlignedAddr >= AlignedEnd)
    return false;

  const uptr AlignedSize = AlignedEnd - AlignedAddr;
  return madvise(reinterpret_cast<void *>(AlignedAddr), AlignedSize,
                 MADV_REMOVE) == 0;
}

// ---------------------------------------------------------------------------
// SharedArena::init
// ---------------------------------------------------------------------------

bool SharedArena::init(u32 Id) {
  CoreId   = Id;
  BaseAddr = kSharedArenaBaseAddr + static_cast<uptr>(Id) * kArenaCapacityPerCore;
  DataBase = BaseAddr + kArenaHeaderSize;

  char ShmName[64];
  snprintf(ShmName, sizeof(ShmName), "/scudo_arena_%u", Id);

  int Fd = shm_open(ShmName, O_CREAT | O_RDWR, 0600);
  if (Fd < 0)
    return false;

  if (ftruncate(Fd, static_cast<off_t>(kArenaCapacityPerCore)) != 0) {
    close(Fd);
    return false;
  }

  void *P = mmap(reinterpret_cast<void *>(BaseAddr), kArenaCapacityPerCore,
                 PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_FIXED_NOREPLACE | MAP_NORESERVE, Fd, 0);
  if (P == MAP_FAILED) {
    close(Fd);
    return false;
  }

  ShmFd = Fd;
  Hdr   = reinterpret_cast<SharedArenaHeader *>(BaseAddr);

  const uptr PageSize = getPageSizeCached();
  const u32 TotalDataPages =
      static_cast<u32>((kArenaCapacityPerCore - kArenaHeaderSize) / PageSize);

  // CAS Version 0 → 1：首次初始化者负责写入元数据。
  u32 Expected = 0u;
  if (atomic_compare_exchange_strong(&Hdr->Version, &Expected, 1u,
                                      memory_order_acq_rel)) {
    atomic_store(&Hdr->Lock, 0u, memory_order_relaxed);
    Hdr->BumpOffsetInPages    = 0;
    Hdr->TotalDataPages       = TotalDataPages;
    Hdr->FreeListHeadPageOff  = kFreeListEnd;
    Hdr->FreeCount            = 0;
    Hdr->TotalDonatedBytes    = 0;
    Hdr->TotalRetrievedBytes  = 0;
    Hdr->DonateCount          = 0;
    Hdr->RetrieveCount        = 0;
    atomic_store(&Hdr->Version, 2u, memory_order_release);
  } else {
    while (atomic_load(&Hdr->Version, memory_order_acquire) < 2u)
      ;
  }

  Initialized = true;
  return true;
}

// ---------------------------------------------------------------------------
// SharedArena::store — VA 有序插入 + 前后合并（DESIGN.md §2.2）
//
// 将一个块归还到侵入式空闲链表。按 VA 地址找到插入位置，写入
// FreeBlockHeader，并尝试与前后相邻的空闲块合并。
// 侵入式链表无容量上限，永远成功。
// ---------------------------------------------------------------------------

void SharedArena::store(uptr CommitBase, uptr CommitSize) {
  if (UNLIKELY(!Initialized || CommitSize == 0))
    return;

  const uptr PageSize = getPageSizeCached();
  const u32 NewPageOff = addrToPageOff(CommitBase);
  const u32 NewPages   = static_cast<u32>(CommitSize / PageSize);

  lock();

  // 按 VA 顺序查找插入位置：找到第一个 PageOff > NewPageOff 的节点，
  // 新块插入到它的前面（PrevOff 和 NextOff 之间）。
  u32 PrevOff = kFreeListEnd;
  u32 NextOff = Hdr->FreeListHeadPageOff;
  while (NextOff != kFreeListEnd) {
    if (NextOff > NewPageOff)
      break;
    PrevOff = NextOff;
    NextOff = getFreeBlock(NextOff)->NextPageOff;
  }

  // 写入新空闲块头部
  FreeBlockHeader *NewBlk = getFreeBlock(NewPageOff);
  NewBlk->PrevPageOff = PrevOff;
  NewBlk->NextPageOff = NextOff;
  NewBlk->SizeInPages = NewPages;
  NewBlk->Magic       = kFreeBlockMagic;

  // 更新前驱和后继的指针
  if (PrevOff != kFreeListEnd)
    getFreeBlock(PrevOff)->NextPageOff = NewPageOff;
  else
    Hdr->FreeListHeadPageOff = NewPageOff;

  if (NextOff != kFreeListEnd)
    getFreeBlock(NextOff)->PrevPageOff = NewPageOff;

  Hdr->FreeCount++;

  // --- 合并后继 ---
  if (NextOff != kFreeListEnd && NewPageOff + NewPages == NextOff) {
    FreeBlockHeader *NBlk = getFreeBlock(NextOff);
    NewBlk->SizeInPages += NBlk->SizeInPages;
    NewBlk->NextPageOff  = NBlk->NextPageOff;
    if (NBlk->NextPageOff != kFreeListEnd)
      getFreeBlock(NBlk->NextPageOff)->PrevPageOff = NewPageOff;
    NBlk->Magic = 0;
    Hdr->FreeCount--;
  }

  // --- 合并前驱 ---
  if (PrevOff != kFreeListEnd) {
    FreeBlockHeader *PBlk = getFreeBlock(PrevOff);
    if (PrevOff + PBlk->SizeInPages == NewPageOff) {
      PBlk->SizeInPages += NewBlk->SizeInPages;
      PBlk->NextPageOff  = NewBlk->NextPageOff;
      if (NewBlk->NextPageOff != kFreeListEnd)
        getFreeBlock(NewBlk->NextPageOff)->PrevPageOff = PrevOff;
      NewBlk->Magic = 0;
      Hdr->FreeCount--;
    }
  }

  Hdr->TotalDonatedBytes += CommitSize;
  Hdr->DonateCount++;

  unlock();
  sharedArenaTrace("store core=%u commit_base=0x%zx commit_size=%zu page_off=%u pages=%u",
                   CoreId, CommitBase, CommitSize, NewPageOff, NewPages);
}

// ---------------------------------------------------------------------------
// SharedArena::retrieve — best-fit freelist + bump fallback（DESIGN.md §2.2）
//
// 分配优先级：
//  1. 扫描侵入式空闲链表做 best-fit，命中则取出（支持尾部切割）。
//  2. 未命中则用 Bump 指针从数据区尾部分配（page fault 按需提交）。
//  3. 均无可用则返回 false，上层回退到 mmap。
// ---------------------------------------------------------------------------

bool SharedArena::retrieve(uptr Size, uptr Alignment, uptr HeadersSize,
                            uptr &OutCommitBase, uptr &OutCommitSize,
                            uptr &OutEntryHeaderPos) {
  if (UNLIKELY(!Initialized))
    return false;

  const uptr PageSize    = getPageSizeCached();
  const uptr NeededSize  = roundUp(Size + HeadersSize, PageSize);
  const u32  NeededPages = static_cast<u32>(NeededSize / PageSize);

  lock();

  uptr AllocBase = 0;
  uptr AllocSize = 0;

  // --- Phase 1: Best-fit 扫描侵入式空闲链表 ---
  u32  BestOff   = kFreeListEnd;
  u32  BestPages = UINT32_MAX;
  {
    u32 Cur = Hdr->FreeListHeadPageOff;
    while (Cur != kFreeListEnd) {
      FreeBlockHeader *Blk = getFreeBlock(Cur);
      if (Blk->Magic == kFreeBlockMagic && Blk->SizeInPages >= NeededPages) {
        if (Blk->SizeInPages < BestPages) {
          BestPages = Blk->SizeInPages;
          BestOff   = Cur;
          if (BestPages == NeededPages)
            break;
        }
      }
      Cur = Blk->NextPageOff;
    }
  }

  if (BestOff != kFreeListEnd) {
    FreeBlockHeader *Best = getFreeBlock(BestOff);
    const u32 Remainder = Best->SizeInPages - NeededPages;

    if (Remainder == 0) {
      // 完美适配：整块摘出
      if (Best->PrevPageOff != kFreeListEnd)
        getFreeBlock(Best->PrevPageOff)->NextPageOff = Best->NextPageOff;
      else
        Hdr->FreeListHeadPageOff = Best->NextPageOff;
      if (Best->NextPageOff != kFreeListEnd)
        getFreeBlock(Best->NextPageOff)->PrevPageOff = Best->PrevPageOff;
      Best->Magic = 0;
      Hdr->FreeCount--;
      AllocBase = pageOffToAddr(BestOff);
      AllocSize = NeededSize;
    } else {
      // 尾部切割：保留前段，分配后段
      u32 AllocOff = BestOff + Remainder;
      Best->SizeInPages = Remainder;
      AllocBase = pageOffToAddr(AllocOff);
      AllocSize = NeededSize;
    }
  }

  // --- Phase 2: Bump 指针分配 ---
  if (AllocBase == 0) {
    const u32 BumpOff = Hdr->BumpOffsetInPages;
    if (BumpOff + NeededPages > Hdr->TotalDataPages) {
      unlock();
      return false;
    }
    AllocBase = pageOffToAddr(BumpOff);
    AllocSize = NeededSize;
    Hdr->BumpOffsetInPages = BumpOff + NeededPages;
  }

  // 计算 LargeBlock::Header 落点
  const uptr AllocPos  = roundDown(AllocBase + AllocSize - Size, Alignment);
  const uptr HeaderPos = AllocPos - HeadersSize;

  if (UNLIKELY(HeaderPos < AllocBase ||
               AllocPos > AllocBase + AllocSize)) {
    unlock();
    return false;
  }

  Hdr->TotalRetrievedBytes += AllocSize;
  Hdr->RetrieveCount++;

  unlock();

  sharedArenaTrace("retrieve core=%u request_size=%zu alignment=%zu commit_base=0x%zx commit_size=%zu header_pos=0x%zx",
                   CoreId, Size, Alignment, AllocBase, AllocSize, HeaderPos);

  OutCommitBase     = AllocBase;
  OutCommitSize     = AllocSize;
  OutEntryHeaderPos = HeaderPos;
  return true;
}

// ---------------------------------------------------------------------------
// SharedArena::getStats
// ---------------------------------------------------------------------------

void SharedArena::getStats(uptr &OutFreeCount, uptr &OutDonatedBytes,
                            uptr &OutRetrievedBytes) const {
  if (!Initialized) {
    OutFreeCount = OutDonatedBytes = OutRetrievedBytes = 0;
    return;
  }
  OutFreeCount      = Hdr->FreeCount;
  OutDonatedBytes   = Hdr->TotalDonatedBytes;
  OutRetrievedBytes = Hdr->TotalRetrievedBytes;
}

// ---------------------------------------------------------------------------
// SharedArenaPool::getOwningArena（DESIGN.md §2.4 跨核迁移释放路径）
//
// 根据块的绝对 VA 计算其归属的 Arena（即最初分配该 VA 的核心）。
// 用于 deallocate()：将释放的块存入归属 Arena 的空闲列表，而非
// 当前执行核心的 Arena，确保每个 Arena 仅包含自身 VA 范围内的块。
// ---------------------------------------------------------------------------

SharedArena *SharedArenaPool::getOwningArena(uptr Va) {
  if (!Initialized || !isArenaAddr(Va))
    return nullptr;
  const u32 Core = static_cast<u32>(
      (Va - kSharedArenaBaseAddr) / kArenaCapacityPerCore);
  if (Core >= NumCores)
    return nullptr;
  SharedArena *A = &Arenas[Core];
  return A->isInitialized() ? A : nullptr;
}

// ---------------------------------------------------------------------------
// 内存压力水位线检测（DESIGN.md §2.1）
//
// 通过读取 /proc/meminfo 获取 MemTotal 和 MemAvailable，计算系统内存
// 使用率。当使用率超过 kMemoryPressureThresholdPercent 时，判定为
// 高内存压力，激活 Arena 分配路径。
//
// 使用 open/read/close 而非 fopen 以避免在分配器内部触发 malloc 重入。
// ---------------------------------------------------------------------------

static uptr parseProcMemInfoValue(const char *Buf, const char *FieldName) {
  const size_t FieldLen = strlen(FieldName);
  const char *P = Buf;
  while ((P = strstr(P, FieldName)) != nullptr) {
    if (P == Buf || *(P - 1) == '\n') {
      P += FieldLen;
      while (*P == ':' || *P == ' ' || *P == '\t')
        P++;
      uptr Value = 0;
      while (*P >= '0' && *P <= '9') {
        Value = Value * 10 + static_cast<uptr>(*P - '0');
        P++;
      }
      return Value; // kB
    }
    P++;
  }
  return 0;
}

bool SharedArenaPool::checkMemoryPressure() {
  int Fd = open("/proc/meminfo", O_RDONLY);
  if (Fd < 0)
    return false;

  char Buf[2048];
  ssize_t N = read(Fd, Buf, sizeof(Buf) - 1);
  close(Fd);
  if (N <= 0)
    return false;
  Buf[N] = '\0';

  const uptr MemTotalKB = parseProcMemInfoValue(Buf, "MemTotal");
  const uptr MemAvailableKB = parseProcMemInfoValue(Buf, "MemAvailable");

  if (MemTotalKB == 0)
    return false;

  const uptr UsagePercent =
      (MemTotalKB - MemAvailableKB) * 100 / MemTotalKB;
  return UsagePercent >= kMemoryPressureThresholdPercent;
}

bool SharedArenaPool::shouldUseArena() {
  if (!Initialized)
    return false;

  if (sharedArenaForceEnabled()) {
    static const bool Logged = []() {
      sharedArenaTrace("forcing shared arena path for testing");
      return true;
    }();
    (void)Logged;
    ArenaActive = true;
    return true;
  }

  const u32 Count =
      atomic_fetch_add(&AllocCounter, 1u, memory_order_relaxed);
  if ((Count & (kPressureCheckInterval - 1)) == 0)
    ArenaActive = checkMemoryPressure();

  return ArenaActive;
}

// ---------------------------------------------------------------------------
// SharedArenaPool::init
// ---------------------------------------------------------------------------

// 外层调用保证了该init函数只会被一个线程调用，不需要做线程安全分析
// 每个进程都会有唯一一个线程调用该函数，创建shared fd
void SharedArenaPool::init() NO_THREAD_SAFETY_ANALYSIS{
  if (Initialized)
    return;

  NumCores = getNumberOfCPUs();
  if (NumCores == 0)
    NumCores = 1;
  if (NumCores > kArenaMaxCores)
    NumCores = kArenaMaxCores;

  // 清理上一次进程会话遗留的共享内存。
  // /dev/shm (tmpfs) 上的文件在进程退出后仍然存在，其中的元数据
  //（freelist head、entries 等）对应旧进程的状态，直接复用会导致崩溃。
  // 跨进程共享场景下，首进程调用 init() 创建新 Arena，后续进程应通过
  // 独立的 attach() 路径加入（attach 不 unlink，本版本尚未实现）。
  for (u32 I = 0; I < NumCores; I++) {
    char ShmName[64];
    snprintf(ShmName, sizeof(ShmName), "/scudo_arena_%u", I);
    shm_unlink(ShmName);
  }

  // 为每个 CPU 核心初始化一个 Arena。
  // 若某个 Arena 初始化失败，则 Pool 的 Initialized 也标记为 false, 走原来的MemAllocaCache路径。
  bool allOk = true;
  for (u32 I = 0; I < NumCores; I++) {
    if (!Arenas[I].init(I))
      allOk = false;
  }
  Initialized = allOk;
}

// ---------------------------------------------------------------------------
// rseqGetCpuId — 通过 rseq TLS 零系统调用获取当前 CPU ID（DESIGN.md §2.2）
//
// glibc 2.35+ 在线程创建时自动调用 rseq(2) 注册，内核在每次调度时更新
// 线程 TLS 中 struct rseq::cpu_id 字段。直接从 TLS 读取该字段，耗时
// 约 1-2 ns（一次内存访问），比 sched_getcpu() 的 vdso 路径快 ~10 倍。
//
// 若 rseq 不可用（__rseq_size == 0 或老版本 glibc），返回 -1。
// ---------------------------------------------------------------------------

static int rseqGetCpuId() {
  if (&__rseq_size == nullptr || __rseq_size == 0)
    return -1;

  // 获取线程指针，加上 __rseq_offset 得到 struct rseq 地址
#if defined(__x86_64__)
  uintptr_t Tp;
  __asm__ volatile("movq %%fs:0, %0" : "=r"(Tp));
#elif defined(__aarch64__)
  uintptr_t Tp;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(Tp));
#else
  return -1;
#endif

  const struct rseq *Rs = reinterpret_cast<const struct rseq *>(
      reinterpret_cast<const char *>(Tp) + __rseq_offset);

  // cpu_id 由内核原子更新，volatile 读取保证不被编译器缓存
  const int CpuId = static_cast<int>(
      *reinterpret_cast<const volatile __u32 *>(&Rs->cpu_id));

  if (CpuId < 0)
    return -1;

  return CpuId;
}

// ---------------------------------------------------------------------------
// SharedArenaPool::getCurrentArena
//
// 优先通过 rseq 获取 CPU ID（零系统调用），不可用时回退到 sched_getcpu()。
// ---------------------------------------------------------------------------

SharedArena *SharedArenaPool::getCurrentArena() {
  if (!Initialized)
    return nullptr;

  int Cpu = rseqGetCpuId();
  if (Cpu < 0)
    Cpu = sched_getcpu();
  if (Cpu < 0 || static_cast<u32>(Cpu) >= NumCores)
    return nullptr;

  SharedArena *A = &Arenas[static_cast<u32>(Cpu)];
  return A->isInitialized() ? A : nullptr;
}

} // namespace scudo

#endif // SCUDO_LINUX
