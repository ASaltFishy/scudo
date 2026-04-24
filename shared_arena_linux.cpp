//===-- shared_arena_linux.cpp ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Linux/Android 平台的共享内存池实现。
//
// 核心机制：
//  1. Linux 使用 shm_open("/scudo_arena_N") 创建具名共享内存，任意进程均可
//     按名打开。
//     Android 使用 memfd / ASharedMemory 创建 backing fd，并通过父进程
//     继承 fd 让后续 exec 进程 attach。
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
#include <limits.h>
#include <sched.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#if SCUDO_ANDROID
#if defined(__has_include)
#if __has_include(<android/sharedmem.h>)
#include <android/sharedmem.h>
#define SCUDO_SHARED_ARENA_HAS_ASHAREDMEM 1
#endif
#endif
#ifndef SCUDO_SHARED_ARENA_HAS_ASHAREDMEM
#define SCUDO_SHARED_ARENA_HAS_ASHAREDMEM 0
#endif
#endif

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

#ifndef PR_SET_MEMORY_DELEGATION_LOG
#define PR_SET_MEMORY_DELEGATION_LOG 74
#endif

namespace scudo {

static atomic_u32 SharedArenaForceForTesting = {};
static atomic_u32 SharedArenaTraceCount = {};

static SharedArenaPool SharedArenaPoolSingleton;

SharedArenaPool &SharedArenaPool::getInstance() {
  return SharedArenaPoolSingleton;
}

static bool sharedArenaEnvEnabled(const char *Name) {
  const char *Value = getenv(Name);
  return Value != nullptr && Value[0] != '\0' && Value[0] != '0';
}

// Cache environment variables once per process to avoid concurrent getenv()
// calls across threads (thread-safety). We still allow unit tests to setenv()
// in child processes before any allocation, then the first cache init will read
// the updated env values.
static atomic_u32 SharedArenaEnvCacheState = {};
static atomic_u32 SharedArenaEnvAttach = {};
static atomic_u32 SharedArenaEnvForce = {};
static atomic_u32 SharedArenaEnvTrace = {};

static void initSharedArenaEnvCacheOnce() {
  // 0: uninitialized, 1: initialized
  u32 Expected = 0u;
  if (atomic_compare_exchange_strong(&SharedArenaEnvCacheState, &Expected, 1u,
                                      memory_order_acq_rel)) {
    // First thread initializes cached values.
    atomic_store(&SharedArenaEnvAttach, sharedArenaEnvEnabled("SCUDO_SHARED_ARENA_ATTACH")
                                           ? 1u
                                           : 0u,
                memory_order_relaxed);
    atomic_store(&SharedArenaEnvForce, sharedArenaEnvEnabled("SCUDO_SHARED_ARENA_FORCE")
                                            ? 1u
                                            : 0u,
                memory_order_relaxed);
    atomic_store(&SharedArenaEnvTrace, sharedArenaEnvEnabled("SCUDO_SHARED_ARENA_TRACE")
                                            ? 1u
                                            : 0u,
                memory_order_relaxed);
    return;
  }

  // Other threads spin until initialization completes.
  while (atomic_load(&SharedArenaEnvCacheState, memory_order_acquire) == 0u)
    ;
}

static bool sharedArenaAttachEnabled() {
  initSharedArenaEnvCacheOnce();
  return atomic_load(&SharedArenaEnvAttach, memory_order_relaxed) != 0u;
}

#if SCUDO_ANDROID
// Android 通过两阶段协议完成无关进程 attach：
//   1. CREATING：单一 creator 进程持有 creator lock，并创建全部 arena backing。
//   2. READY：creator 启动 broker，对外按名字分发 arena fd；其他进程仅 attach。
//
// 这样可以避免“多个进程同时发现 broker 不存在，然后各自创建不同 backing”
// 的竞态。
static constexpr char kSharedArenaBrokerName[] = "scudo_shared_arena_broker";
static constexpr char kSharedArenaCreatorName[] = "scudo_shared_arena_creator";
static constexpr u32 kSharedArenaBrokerMagic = 0x5341524EU; // "SARN"
static constexpr u32 kSharedArenaInitRetryCount = 5000;
static constexpr useconds_t kSharedArenaInitRetrySleepUs = 1000;

enum class SharedArenaAndroidInitMode : u8 {
  Unknown = 0,
  Create  = 1,
  Attach  = 2,
};

static SharedArenaAndroidInitMode SharedArenaInitMode =
    SharedArenaAndroidInitMode::Unknown;
static int SharedArenaCreatorLockFd = -1;

struct SharedArenaBrokerRequest {
  u32 Magic;
  u32 CoreId;
};

struct SharedArenaBrokerReply {
  s32 Status;
  u32 Reserved;
};

static void sharedArenaBrokerSockaddr(sockaddr_un &Addr, socklen_t &AddrLen) {
  memset(&Addr, 0, sizeof(Addr));
  Addr.sun_family = AF_UNIX;
  Addr.sun_path[0] = '\0';
  memcpy(Addr.sun_path + 1, kSharedArenaBrokerName,
         sizeof(kSharedArenaBrokerName) - 1);
  AddrLen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 +
                                   sizeof(kSharedArenaBrokerName) - 1);
}

static void sharedArenaCreatorSockaddr(sockaddr_un &Addr, socklen_t &AddrLen) {
  memset(&Addr, 0, sizeof(Addr));
  Addr.sun_family = AF_UNIX;
  Addr.sun_path[0] = '\0';
  memcpy(Addr.sun_path + 1, kSharedArenaCreatorName,
         sizeof(kSharedArenaCreatorName) - 1);
  AddrLen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 +
                                   sizeof(kSharedArenaCreatorName) - 1);
}

static bool sharedArenaWriteExact(int Fd, const void *Buf, size_t Size) {
  const char *P = reinterpret_cast<const char *>(Buf);
  while (Size != 0) {
    const ssize_t N = write(Fd, P, Size);
    if (N < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (N == 0)
      return false;
    P += N;
    Size -= static_cast<size_t>(N);
  }
  return true;
}

static bool sharedArenaReadExact(int Fd, void *Buf, size_t Size) {
  char *P = reinterpret_cast<char *>(Buf);
  while (Size != 0) {
    const ssize_t N = read(Fd, P, Size);
    if (N < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (N == 0)
      return false;
    P += N;
    Size -= static_cast<size_t>(N);
  }
  return true;
}

static int sharedArenaConnectBroker() {
  const int Sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (Sock < 0) {
    sharedArenaTrace("broker connect socket failed errno=%d", errno);
    return -1;
  }

  sockaddr_un Addr;
  socklen_t AddrLen;
  sharedArenaBrokerSockaddr(Addr, AddrLen);
  if (connect(Sock, reinterpret_cast<sockaddr *>(&Addr), AddrLen) != 0) {
    sharedArenaTrace("broker connect failed errno=%d", errno);
    close(Sock);
    return -1;
  }
  return Sock;
}

static bool sharedArenaBrokerAvailable() {
  const int Sock = sharedArenaConnectBroker();
  if (Sock < 0)
    return false;
  close(Sock);
  return true;
}

static int sharedArenaTryAcquireCreatorLock() {
  const int Sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (Sock < 0) {
    sharedArenaTrace("creator lock socket failed errno=%d", errno);
    return -1;
  }

  sockaddr_un Addr;
  socklen_t AddrLen;
  sharedArenaCreatorSockaddr(Addr, AddrLen);
  if (bind(Sock, reinterpret_cast<sockaddr *>(&Addr), AddrLen) == 0) {
    sharedArenaTrace("creator lock acquired");
    return Sock;
  }

  const int SavedErrno = errno;
  sharedArenaTrace("creator lock bind failed errno=%d", SavedErrno);
  close(Sock);
  errno = SavedErrno;
  return -1;
}

static void sharedArenaReleaseCreatorLock() {
  if (SharedArenaCreatorLockFd >= 0) {
    close(SharedArenaCreatorLockFd);
    SharedArenaCreatorLockFd = -1;
  }
}

static bool sharedArenaSelectAndroidInitMode() {
  if (sharedArenaAttachEnabled()) {
    SharedArenaInitMode = SharedArenaAndroidInitMode::Attach;
    sharedArenaTrace("android init mode=attach (env)");
    return true;
  }

  for (u32 Attempt = 0; Attempt < kSharedArenaInitRetryCount; ++Attempt) {
    if (sharedArenaBrokerAvailable()) {
      SharedArenaInitMode = SharedArenaAndroidInitMode::Attach;
      sharedArenaTrace("android init mode=attach (broker ready)");
      return true;
    }

    const int CreatorFd = sharedArenaTryAcquireCreatorLock();
    if (CreatorFd >= 0) {
      SharedArenaCreatorLockFd = CreatorFd;
      SharedArenaInitMode = SharedArenaAndroidInitMode::Create;
      sharedArenaTrace("android init mode=create");
      return true;
    }

    if (errno != EADDRINUSE) {
      sharedArenaTrace("android init mode select failed errno=%d", errno);
      return false;
    }

    usleep(kSharedArenaInitRetrySleepUs);
  }

  sharedArenaTrace("android init mode select timed out waiting for broker");
  return false;
}

static bool sharedArenaSendBrokerReply(int Sock, int Status, int FdToSend) {
  SharedArenaBrokerReply Reply = {};
  Reply.Status = Status;

  iovec Iov = {};
  Iov.iov_base = &Reply;
  Iov.iov_len = sizeof(Reply);

  alignas(struct cmsghdr) char Control[CMSG_SPACE(sizeof(int))];
  memset(Control, 0, sizeof(Control));

  msghdr Msg = {};
  Msg.msg_iov = &Iov;
  Msg.msg_iovlen = 1;
  if (FdToSend >= 0) {
    Msg.msg_control = Control;
    Msg.msg_controllen = sizeof(Control);
    cmsghdr *Cmsg = CMSG_FIRSTHDR(&Msg);
    Cmsg->cmsg_level = SOL_SOCKET;
    Cmsg->cmsg_type = SCM_RIGHTS;
    Cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    *reinterpret_cast<int *>(CMSG_DATA(Cmsg)) = FdToSend;
    Msg.msg_controllen = Cmsg->cmsg_len;
  }

  while (sendmsg(Sock, &Msg, MSG_NOSIGNAL) < 0) {
    if (errno == EINTR)
      continue;
    return false;
  }
  return true;
}

static int sharedArenaRequestBrokerFd(u32 CoreId) {
  const int Sock = sharedArenaConnectBroker();
  if (Sock < 0) {
    sharedArenaTrace("request broker fd core=%u connect failed", CoreId);
    return -1;
  }

  SharedArenaBrokerRequest Request = {};
  Request.Magic = kSharedArenaBrokerMagic;
  Request.CoreId = CoreId;
  if (!sharedArenaWriteExact(Sock, &Request, sizeof(Request))) {
    sharedArenaTrace("request broker fd core=%u write failed errno=%d", CoreId,
                     errno);
    close(Sock);
    return -1;
  }

  SharedArenaBrokerReply Reply = {};
  iovec Iov = {};
  Iov.iov_base = &Reply;
  Iov.iov_len = sizeof(Reply);

  alignas(struct cmsghdr) char Control[CMSG_SPACE(sizeof(int))];
  memset(Control, 0, sizeof(Control));

  msghdr Msg = {};
  Msg.msg_iov = &Iov;
  Msg.msg_iovlen = 1;
  Msg.msg_control = Control;
  Msg.msg_controllen = sizeof(Control);

  ssize_t N;
  do {
    N = recvmsg(Sock, &Msg, 0);
  } while (N < 0 && errno == EINTR);

  int ReceivedFd = -1;
  if (N == static_cast<ssize_t>(sizeof(Reply)) && Reply.Status == 0) {
    for (cmsghdr *Cmsg = CMSG_FIRSTHDR(&Msg); Cmsg != nullptr;
         Cmsg = CMSG_NXTHDR(&Msg, Cmsg)) {
      if (Cmsg->cmsg_level == SOL_SOCKET && Cmsg->cmsg_type == SCM_RIGHTS &&
          Cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
        ReceivedFd = *reinterpret_cast<int *>(CMSG_DATA(Cmsg));
        break;
      }
    }
  }

  if (ReceivedFd < 0)
    sharedArenaTrace("request broker fd core=%u failed status=%d recv=%zd errno=%d",
                     CoreId, Reply.Status, N, errno);
  else
    sharedArenaTrace("request broker fd core=%u success fd=%d", CoreId,
                     ReceivedFd);
  close(Sock);
  return ReceivedFd;
}

static void sharedArenaFdEnvName(u32 CoreId, char *Buf, size_t BufSize) {
  snprintf(Buf, BufSize, "SCUDO_SHARED_ARENA_FD_%u", CoreId);
}

static bool sharedArenaGetInheritedFd(u32 CoreId, int &OutFd) {
  char Name[64];
  sharedArenaFdEnvName(CoreId, Name, sizeof(Name));
  const char *Value = getenv(Name);
  if (Value == nullptr || Value[0] == '\0')
    return false;

  char *End = nullptr;
  errno = 0;
  const long Parsed = strtol(Value, &End, 10);
  if (errno != 0 || End == Value || (End != nullptr && *End != '\0') ||
      Parsed < 0 || Parsed > INT_MAX) {
    return false;
  }

  const int Fd = static_cast<int>(Parsed);
  if (fcntl(Fd, F_GETFD) < 0)
    return false;

  OutFd = Fd;
  return true;
}

static bool sharedArenaExportFd(u32 CoreId, int Fd) {
#if SCUDO_ANDROID
  char Name[64];
  char Value[32];
  sharedArenaFdEnvName(CoreId, Name, sizeof(Name));
  snprintf(Value, sizeof(Value), "%d", Fd);
  return setenv(Name, Value, 1) == 0;
#else
  (void)CoreId;
  (void)Fd;
  return true;
#endif
}

static bool sharedArenaClearCloseOnExec(int Fd) {
  const int Flags = fcntl(Fd, F_GETFD);
  if (Flags < 0)
    return false;
  if ((Flags & FD_CLOEXEC) == 0)
    return true;
  return fcntl(Fd, F_SETFD, Flags & ~FD_CLOEXEC) == 0;
}

static int sharedArenaCreateAndroidBacking(u32 CoreId) {
  char Name[64];
  snprintf(Name, sizeof(Name), "scudo_arena_%u", CoreId);

#if defined(SYS_memfd_create)
  int Fd = static_cast<int>(syscall(SYS_memfd_create, Name, 0));
  if (Fd >= 0) {
    if (ftruncate(Fd, static_cast<off_t>(kArenaCapacityPerCore)) == 0) {
      sharedArenaTrace("create backing core=%u via memfd fd=%d", CoreId, Fd);
      return Fd;
    }
    sharedArenaTrace("create backing core=%u memfd ftruncate failed errno=%d",
                     CoreId, errno);
    close(Fd);
    return -1;
  }
  sharedArenaTrace("create backing core=%u memfd_create failed errno=%d", CoreId,
                   errno);
#endif

#if SCUDO_SHARED_ARENA_HAS_ASHAREDMEM
  const int AshmemFd =
      ASharedMemory_create(Name, static_cast<size_t>(kArenaCapacityPerCore));
  sharedArenaTrace("create backing core=%u via ASharedMemory fd=%d errno=%d",
                   CoreId, AshmemFd, errno);
  return AshmemFd;
#else
  (void)Name;
  sharedArenaTrace("create backing core=%u unavailable: no memfd/ASharedMemory",
                   CoreId);
  return -1;
#endif
}

static void sharedArenaBrokerLoop(u32 NumCores, const SharedArena *Arenas) {
  const int ListenFd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (ListenFd < 0) {
    sharedArenaTrace("broker loop socket failed errno=%d", errno);
    _exit(1);
  }

  sockaddr_un Addr;
  socklen_t AddrLen;
  sharedArenaBrokerSockaddr(Addr, AddrLen);
  if (bind(ListenFd, reinterpret_cast<sockaddr *>(&Addr), AddrLen) != 0) {
    sharedArenaTrace("broker loop bind failed errno=%d", errno);
    close(ListenFd);
    _exit(errno == EADDRINUSE ? 0 : 1);
  }
  if (listen(ListenFd, 4) != 0) {
    sharedArenaTrace("broker loop listen failed errno=%d", errno);
    close(ListenFd);
    _exit(1);
  }
  sharedArenaTrace("broker loop ready num_cores=%u", NumCores);

  for (;;) {
    const int Client = accept4(ListenFd, nullptr, nullptr, SOCK_CLOEXEC);
    if (Client < 0) {
      if (errno == EINTR)
        continue;
      break;
    }

    SharedArenaBrokerRequest Request = {};
    if (!sharedArenaReadExact(Client, &Request, sizeof(Request))) {
      close(Client);
      continue;
    }

    if (Request.Magic != kSharedArenaBrokerMagic ||
        Request.CoreId >= NumCores) {
      (void)sharedArenaSendBrokerReply(Client, -1, -1);
      close(Client);
      continue;
    }

    const int ArenaFd = Arenas[Request.CoreId].getShmFd();
    sharedArenaTrace("broker serve core=%u fd=%d", Request.CoreId, ArenaFd);
    (void)sharedArenaSendBrokerReply(Client, ArenaFd >= 0 ? 0 : -1, ArenaFd);
    close(Client);
  }

  close(ListenFd);
  _exit(1);
}

static bool sharedArenaStartBroker(u32 NumCores, const SharedArena *Arenas) {
  const pid_t Pid = fork();
  if (Pid < 0) {
    sharedArenaTrace("start broker fork failed errno=%d", errno);
    return false;
  }
  if (Pid == 0) {
    (void)setsid();
    const int NullFd = open("/dev/null", O_RDWR);
    if (NullFd >= 0) {
      (void)dup2(NullFd, STDIN_FILENO);
      (void)dup2(NullFd, STDOUT_FILENO);
      (void)dup2(NullFd, STDERR_FILENO);
      if (NullFd > STDERR_FILENO)
        close(NullFd);
    }
    sharedArenaBrokerLoop(NumCores, Arenas);
    _exit(1);
  }
  sharedArenaTrace("start broker pid=%d", Pid);

  for (u32 Attempt = 0; Attempt < 50; ++Attempt) {
    if (sharedArenaBrokerAvailable())
      return true;
    usleep(1000);
  }
  sharedArenaTrace("start broker timeout waiting ready");
  return false;
}
#endif

bool sharedArenaForceEnabled() {
  initSharedArenaEnvCacheOnce();
  const u32 Override =
      atomic_load(&SharedArenaForceForTesting, memory_order_relaxed);
  if (Override == 2u)
    return true;
  if (Override == 1u)
    return false;
  return atomic_load(&SharedArenaEnvForce, memory_order_relaxed) != 0u;
}

void setSharedArenaForceForTesting(bool Enabled) {
  atomic_store(&SharedArenaForceForTesting, Enabled ? 2u : 1u,
               memory_order_relaxed);
}

void clearSharedArenaForceForTesting() {
  atomic_store(&SharedArenaForceForTesting, 0u, memory_order_relaxed);
}

bool sharedArenaTraceEnabled() {
  initSharedArenaEnvCacheOnce();
  return atomic_load(&SharedArenaEnvTrace, memory_order_relaxed) != 0u;
}

void sharedArenaTrace(const char *Format, ...) {
  if (!sharedArenaTraceEnabled())
    return;

  const u32 Count =
      atomic_fetch_add(&SharedArenaTraceCount, 1u, memory_order_relaxed);
  if (Count >= 64) {
    if (Count == 64) {  
      static constexpr char LimitMsg[] =
          "[scudo][shared_arena] trace limit reached, suppressing further logs\n";
      ssize_t R = write(STDERR_FILENO, LimitMsg, sizeof(LimitMsg) - 1);
      (void)R;
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
  int BodyLen = vsnprintf(Buffer + PrefixLen,
                          sizeof(Buffer) - static_cast<size_t>(PrefixLen),
                          Format, Args);
  va_end(Args);
  if (BodyLen < 0)
    return;

  size_t TotalLen =
      Min(sizeof(Buffer) - 1, static_cast<size_t>(PrefixLen + BodyLen));
  if (TotalLen == 0 || Buffer[TotalLen - 1] != '\n')
    Buffer[TotalLen++] = '\n';
  ssize_t R = write(STDERR_FILENO, Buffer, TotalLen);
  (void)R;
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

bool SharedArena::appendLogLocked(SharedArenaLogOp Op, uptr CommitBase,
                                  uptr CommitSize) {
  // Bench-only no-kernel mode: the kernel does not consume the ring (Head never
  // advances). Mask out all ring operations so the arena path doesn't
  // self-disable due to a full ring. NOT safe for production semantics.
  if (sharedArenaEnvEnabled("SCUDO_SHARED_ARENA_ALLOW_NO_KERNEL"))
    return true;

  DCHECK(LogRing != nullptr);

  const uptr PageSize = getPageSizeCached();
  if (UNLIKELY(CommitSize == 0 || !isAligned(CommitBase, PageSize) ||
               !isAligned(CommitSize, PageSize)))
    return false;

  const u32 Capacity = LogRing->Capacity;
  if (UNLIKELY(Capacity == 0))
    return false;

  const u32 Head = atomic_load(&LogRing->Head, memory_order_acquire);
  const u32 Tail = atomic_load(&LogRing->Tail, memory_order_relaxed);
  if (UNLIKELY(Tail - Head >= Capacity)) {
    atomic_fetch_add(&LogRing->Dropped, 1u, memory_order_relaxed);
    return false;
  }

  SharedArenaLogEntry *Entries = getLogEntries();
  SharedArenaLogEntry &Entry = Entries[Tail % Capacity];
  Entry.Op = static_cast<u8>(Op);
  Entry.Reserved = 0;
  Entry.Reserved2 = 0;
  Entry.StartPage = addrToPageOff(CommitBase);
  Entry.NumPages = static_cast<u32>(CommitSize / PageSize);
  atomic_store(&LogRing->Tail, Tail + 1, memory_order_release);
  return true;
}

bool SharedArena::initLogRing() {
  DCHECK(LogRing == nullptr);

  const uptr PageSize = getPageSizeCached();
  void *RingMem = mmap(nullptr, PageSize, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (RingMem == MAP_FAILED)
    return false;

  memset(RingMem, 0, PageSize);
  LogRing = reinterpret_cast<SharedArenaLogRing *>(RingMem);
  LogRingSize = PageSize;

  const uptr HeaderBytes = sizeof(*LogRing);
  if (UNLIKELY(HeaderBytes >= PageSize)) {
    munmap(RingMem, PageSize);
    LogRing = nullptr;
    LogRingSize = 0;
    return false;
  }

  const u32 Capacity = static_cast<u32>((PageSize - HeaderBytes) /
                                        sizeof(SharedArenaLogEntry));
  if (UNLIKELY(Capacity == 0)) {
    munmap(RingMem, PageSize);
    LogRing = nullptr;
    LogRingSize = 0;
    return false;
  }

  LogRing->Magic = kLogRingMagic;
  LogRing->Version = kLogRingVersion;
  LogRing->Flags = 0;
  LogRing->Capacity = Capacity;
  LogRing->ArenaNrPages = Hdr->TotalDataPages;
  LogRing->RingSize = static_cast<u32>(PageSize);
  atomic_store(&LogRing->Head, 0u, memory_order_relaxed);
  atomic_store(&LogRing->Tail, 0u, memory_order_relaxed);
  atomic_store(&LogRing->Dropped, 0u, memory_order_relaxed);
  return true;
}

bool SharedArena::registerWithKernel() {
  DCHECK(LogRing != nullptr);

  const long Ret =
      prctl(PR_SET_MEMORY_DELEGATION_LOG,
            reinterpret_cast<unsigned long>(LogRing), 0UL,
            static_cast<unsigned long>(CoreId),
            static_cast<unsigned long>(DataBase));
  if (Ret == 0)
    return true;

  // NOTE: DESIGN.md assumes kernel support for the delegation log. For
  // microbench only, allow a best-effort fallback when the kernel does not
  // recognize the prctl (EINVAL). This disables kernel-side ownership tracking
  // and is NOT safe for production use.
  if (errno == EINVAL && sharedArenaEnvEnabled("SCUDO_SHARED_ARENA_ALLOW_NO_KERNEL")) {
    sharedArenaTrace("arena register core=%u: kernel prctl unsupported (EINVAL), "
                     "continuing due to SCUDO_SHARED_ARENA_ALLOW_NO_KERNEL=1",
                     CoreId);
    return true;
  }

  sharedArenaTrace("arena register core=%u data_base=0x%zx ring=0x%zx failed ret=%ld errno=%d",
                   CoreId, DataBase, reinterpret_cast<uptr>(LogRing), Ret,
                   errno);
  return false;
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

  int Fd = -1;
#if SCUDO_ANDROID
  switch (SharedArenaInitMode) {
  case SharedArenaAndroidInitMode::Attach:
    if (!sharedArenaGetInheritedFd(Id, Fd))
      Fd = sharedArenaRequestBrokerFd(Id);
    if (Fd < 0) {
      sharedArenaTrace("arena init core=%u attach failed", Id);
      return false;
    }
    break;
  case SharedArenaAndroidInitMode::Create:
    Fd = sharedArenaCreateAndroidBacking(Id);
    if (Fd < 0) {
      sharedArenaTrace("arena init core=%u create backing failed", Id);
      return false;
    }
    if (!sharedArenaClearCloseOnExec(Fd) || !sharedArenaExportFd(Id, Fd)) {
      sharedArenaTrace("arena init core=%u export fd failed errno=%d", Id,
                       errno);
      close(Fd);
      return false;
    }
    break;
  case SharedArenaAndroidInitMode::Unknown:
  default:
    sharedArenaTrace("arena init core=%u unknown init mode", Id);
    return false;
  }
#else
  char ShmName[64];
  snprintf(ShmName, sizeof(ShmName), "/scudo_arena_%u", Id);

  Fd = shm_open(ShmName, O_CREAT | O_RDWR, 0600);
  if (Fd < 0)
    return false;

  if (ftruncate(Fd, static_cast<off_t>(kArenaCapacityPerCore)) != 0) {
    close(Fd);
    return false;
  }
#endif

  void *P = mmap(reinterpret_cast<void *>(BaseAddr), kArenaCapacityPerCore,
                 PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_FIXED_NOREPLACE | MAP_NORESERVE, Fd, 0);
  if (P == MAP_FAILED) {
    sharedArenaTrace("arena init core=%u mmap failed base=0x%zx errno=%d", Id,
                     BaseAddr, errno);
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

  if (!initLogRing()) {
    munmap(reinterpret_cast<void *>(BaseAddr), kArenaCapacityPerCore);
    close(Fd);
    Hdr = nullptr;
    ShmFd = -1;
    return false;
  }

  if (!registerWithKernel()) {
    munmap(reinterpret_cast<void *>(LogRing), LogRingSize);
    LogRing = nullptr;
    LogRingSize = 0;
    munmap(reinterpret_cast<void *>(BaseAddr), kArenaCapacityPerCore);
    close(Fd);
    Hdr = nullptr;
    ShmFd = -1;
    return false;
  }

  Initialized = true;
  sharedArenaTrace("arena init core=%u success base=0x%zx fd=%d", Id, BaseAddr,
                   Fd);
  return true;
}

void SharedArena::reset() {
  if (UNLIKELY(!Initialized || Hdr == nullptr))
    return;

  const uptr PageSize = getPageSizeCached();
  const u32 TotalDataPages =
      static_cast<u32>((kArenaCapacityPerCore - kArenaHeaderSize) / PageSize);

  lock();
  Hdr->BumpOffsetInPages   = 0;
  Hdr->TotalDataPages      = TotalDataPages;
  Hdr->FreeListHeadPageOff = kFreeListEnd;
  Hdr->FreeCount           = 0;
  Hdr->TotalDonatedBytes   = 0;
  Hdr->TotalRetrievedBytes = 0;
  Hdr->DonateCount         = 0;
  Hdr->RetrieveCount       = 0;
  unlock();

  if (LIKELY(LogRing != nullptr)) {
    LogRing->ArenaNrPages = TotalDataPages;
    atomic_store(&LogRing->Head, 0u, memory_order_relaxed);
    atomic_store(&LogRing->Tail, 0u, memory_order_relaxed);
    atomic_store(&LogRing->Dropped, 0u, memory_order_relaxed);
  }

  const uptr DataSize = kArenaCapacityPerCore - kArenaHeaderSize;
  (void)releasePages(DataBase, DataSize);
}

// ---------------------------------------------------------------------------
// SharedArena::store — VA 有序插入 + 前后合并（DESIGN.md §2.2）
//
// 将一个块归还到侵入式空闲链表。按 VA 地址找到插入位置，写入
// FreeBlockHeader，并尝试与前后相邻的空闲块合并。
// 侵入式链表无容量上限，永远成功。
// ---------------------------------------------------------------------------

bool SharedArena::store(uptr CommitBase, uptr CommitSize) {
  if (UNLIKELY(!Initialized || CommitSize == 0))
    return false;

  const uptr PageSize = getPageSizeCached();
  const u32 NewPageOff = addrToPageOff(CommitBase);
  const u32 NewPages   = static_cast<u32>(CommitSize / PageSize);

  lock();
  if (UNLIKELY(!appendLogLocked(SharedArenaLogOp::Free, CommitBase,
                                CommitSize))) {
    unlock();
    sharedArenaTrace("log append failed on free core=%u base=0x%zx size=%zu",
                     CoreId, CommitBase, CommitSize);
    return false;
  }

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
  return true;
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
  if (UNLIKELY(!appendLogLocked(SharedArenaLogOp::Alloc, AllocBase,
                                AllocSize))) {
    if (BestOff != kFreeListEnd) {
      if (AllocBase == pageOffToAddr(BestOff)) {
        FreeBlockHeader *Best = getFreeBlock(BestOff);
        Best->Magic = kFreeBlockMagic;
        if (Best->PrevPageOff != kFreeListEnd)
          getFreeBlock(Best->PrevPageOff)->NextPageOff = BestOff;
        else
          Hdr->FreeListHeadPageOff = BestOff;
        if (Best->NextPageOff != kFreeListEnd)
          getFreeBlock(Best->NextPageOff)->PrevPageOff = BestOff;
        Hdr->FreeCount++;
      } else {
        FreeBlockHeader *Best = getFreeBlock(BestOff);
        Best->SizeInPages += NeededPages;
      }
    } else {
      Hdr->BumpOffsetInPages -= NeededPages;
    }
    Hdr->TotalRetrievedBytes -= AllocSize;
    Hdr->RetrieveCount--;
    unlock();
    sharedArenaTrace(
        "log append failed on alloc core=%u base=0x%zx size=%zu", CoreId,
        AllocBase, AllocSize);
    return false;
  }

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

  bool ShouldCreateFreshBacking =
#if SCUDO_ANDROID
      false;
#else
      !sharedArenaAttachEnabled();
#endif
#if SCUDO_ANDROID
  SharedArenaInitMode = SharedArenaAndroidInitMode::Unknown;
  sharedArenaReleaseCreatorLock();
  if (!sharedArenaSelectAndroidInitMode()) {
    sharedArenaTrace("pool init failed selecting android init mode");
    Initialized = false;
    return;
  }
  ShouldCreateFreshBacking =
      SharedArenaInitMode == SharedArenaAndroidInitMode::Create;
  sharedArenaTrace("pool init android mode=%u num_cores=%u",
                   static_cast<unsigned>(SharedArenaInitMode), NumCores);
#endif

  // Linux: 清理上一次进程会话遗留的具名共享内存。
  // /dev/shm (tmpfs) 上的文件在进程退出后仍然存在，其中的元数据
  //（freelist head、entries 等）对应旧进程的状态，直接复用会导致崩溃。
  //
  // Android: 只有拿到 creator lock、进入 CREATING 阶段的单一进程允许创建
  // backing；其他进程在 READY 阶段只 attach 到 broker。
  if (ShouldCreateFreshBacking) {
#if !SCUDO_ANDROID
    for (u32 I = 0; I < NumCores; I++) {
      char ShmName[64];
      snprintf(ShmName, sizeof(ShmName), "/scudo_arena_%u", I);
      shm_unlink(ShmName);
    }
#endif
  }

  // 为每个 CPU 核心初始化一个 Arena。
  // 若某个 Arena 初始化失败，则 Pool 的 Initialized 也标记为 false, 走原来的MemAllocaCache路径。
  bool allOk = true;
  for (u32 I = 0; I < NumCores; I++) {
    if (!Arenas[I].init(I))
      allOk = false;
  }
#if SCUDO_ANDROID
  if (allOk && SharedArenaInitMode == SharedArenaAndroidInitMode::Create)
    allOk = sharedArenaStartBroker(NumCores, Arenas);
  sharedArenaReleaseCreatorLock();
  if (!allOk)
    SharedArenaInitMode = SharedArenaAndroidInitMode::Unknown;
#endif
  sharedArenaTrace("pool init finished ready=%d", allOk ? 1 : 0);
  Initialized = allOk;
}

void SharedArenaPool::reset() {
  if (!Initialized)
    return;
  for (u32 I = 0; I < NumCores; ++I) {
    if (Arenas[I].isInitialized())
      Arenas[I].reset();
  }
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
