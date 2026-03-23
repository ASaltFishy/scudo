//===-- secondary_test.cpp --------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "memtag.h"
#include "tests/scudo_unit_test.h"

#include "allocator_config.h"
#include "allocator_config_wrapper.h"
#include "secondary.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <random>
#include <stdio.h>
#include <thread>
#include <vector>

#if SCUDO_LINUX
#include <sched.h>
#include <limits.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

template <typename Config> static scudo::Options getOptionsForConfig() {
  if (!Config::getMaySupportMemoryTagging() ||
      !scudo::archSupportsMemoryTagging() ||
      !scudo::systemSupportsMemoryTagging())
    return {};
  scudo::AtomicOptions AO;
  AO.set(scudo::OptionBit::UseMemoryTagging);
  return AO.load();
}

template <typename Config> static void testSecondaryBasic(void) {
  using SecondaryT = scudo::MapAllocator<scudo::SecondaryConfig<Config>>;
  scudo::Options Options =
      getOptionsForConfig<scudo::SecondaryConfig<Config>>();

  scudo::GlobalStats S;
  S.init();
  std::unique_ptr<SecondaryT> L(new SecondaryT);
  L->init(&S);
  const scudo::uptr Size = 1U << 16;
  void *P = L->allocate(Options, Size);
  EXPECT_NE(P, nullptr);
  memset(P, 'A', Size);
  EXPECT_GE(SecondaryT::getBlockSize(P), Size);
  L->deallocate(Options, P);

  // If the Secondary can't cache that pointer, it will be unmapped.
  if (!L->canCache(Size)) {
    EXPECT_DEATH(
        {
          // Repeat few time to avoid missing crash if it's mmaped by unrelated
          // code.
          for (int i = 0; i < 10; ++i) {
            P = L->allocate(Options, Size);
            L->deallocate(Options, P);
            memset(P, 'A', Size);
          }
        },
        "");
  }

  const scudo::uptr Align = 1U << 16;
  P = L->allocate(Options, Size + Align, Align);
  EXPECT_NE(P, nullptr);
  void *AlignedP = reinterpret_cast<void *>(
      scudo::roundUp(reinterpret_cast<scudo::uptr>(P), Align));
  memset(AlignedP, 'A', Size);
  L->deallocate(Options, P);

  std::vector<void *> V;
  for (scudo::uptr I = 0; I < 32U; I++)
    V.push_back(L->allocate(Options, Size));
  std::shuffle(V.begin(), V.end(), std::mt19937(std::random_device()()));
  while (!V.empty()) {
    L->deallocate(Options, V.back());
    V.pop_back();
  }
  scudo::ScopedString Str;
  L->getStats(&Str);
  Str.output();
  L->unmapTestOnly();
}

struct NoCacheConfig {
  static const bool MaySupportMemoryTagging = false;
  template <typename> using TSDRegistryT = void;
  template <typename> using PrimaryT = void;
  template <typename Config> using SecondaryT = scudo::MapAllocator<Config>;

  struct Secondary {
    template <typename Config>
    using CacheT = scudo::MapAllocatorNoCache<Config>;
  };
};

struct TestConfig {
  static const bool MaySupportMemoryTagging = false;
  template <typename> using TSDRegistryT = void;
  template <typename> using PrimaryT = void;
  template <typename> using SecondaryT = void;

  struct Secondary {
    struct Cache {
      static const scudo::u32 EntriesArraySize = 128U;
      static const scudo::u32 QuarantineSize = 0U;
      static const scudo::u32 DefaultMaxEntriesCount = 64U;
      static const scudo::uptr DefaultMaxEntrySize = 1UL << 20;
      static const scudo::s32 MinReleaseToOsIntervalMs = INT32_MIN;
      static const scudo::s32 MaxReleaseToOsIntervalMs = INT32_MAX;
    };

    template <typename Config> using CacheT = scudo::MapAllocatorCache<Config>;
  };
};

#if SCUDO_LINUX
namespace {

struct SharedArenaTotals {
  scudo::uptr FreeCount = 0;
  scudo::uptr DonatedBytes = 0;
  scudo::uptr RetrievedBytes = 0;
};

static SharedArenaTotals getSharedArenaTotals() {
  SharedArenaTotals Totals;
  scudo::SharedArenaPool &Pool = scudo::SharedArenaPool::getInstance();
  if (!Pool.isReady())
    return Totals;

  for (scudo::u32 I = 0; I < Pool.getNumCores(); ++I) {
    scudo::SharedArena *Arena = Pool.getArena(I);
    if (!Arena || !Arena->isInitialized())
      continue;
    scudo::uptr FreeCount = 0;
    scudo::uptr DonatedBytes = 0;
    scudo::uptr RetrievedBytes = 0;
    Arena->getStats(FreeCount, DonatedBytes, RetrievedBytes);
    Totals.FreeCount += FreeCount;
    Totals.DonatedBytes += DonatedBytes;
    Totals.RetrievedBytes += RetrievedBytes;
  }

  return Totals;
}

static int getUsableArenaCpu() {
  scudo::SharedArenaPool &Pool = scudo::SharedArenaPool::getInstance();
  if (!Pool.isReady() || Pool.getNumCores() == 0)
    return -1;

  cpu_set_t Allowed;
  CPU_ZERO(&Allowed);
  if (sched_getaffinity(0, sizeof(Allowed), &Allowed) != 0)
    return -1;

  for (scudo::u32 Cpu = 0; Cpu < Pool.getNumCores(); ++Cpu) {
    if (CPU_ISSET(static_cast<int>(Cpu), &Allowed))
      return static_cast<int>(Cpu);
  }

  return -1;
}

static bool pinProcessToArenaCpu() {
  const int Cpu = getUsableArenaCpu();
  if (Cpu < 0)
    return false;

  cpu_set_t Set;
  CPU_ZERO(&Set);
  CPU_SET(Cpu, &Set);
  return sched_setaffinity(0, sizeof(Set), &Set) == 0;
}

} // namespace

struct SharedArenaDelegationTest : public Test {
  using Config = scudo::DefaultConfig;
  using LargeAllocator = scudo::MapAllocator<scudo::SecondaryConfig<Config>>;

  void SetUp() override {
    scudo::setSharedArenaForceForTesting(true);
    Allocator->init(nullptr);
    if (!scudo::SharedArenaPool::getInstance().isReady())
      TEST_SKIP("SharedArenaPool not ready");
    if (!pinProcessToArenaCpu())
      TEST_SKIP("No usable CPU managed by SharedArenaPool");
  }

  void TearDown() override {
    Allocator->unmapTestOnly();
    scudo::clearSharedArenaForceForTesting();
  }

  void *allocate(scudo::uptr Size) {
    void *Ptr = Allocator->allocate(Options, Size);
    EXPECT_NE(Ptr, nullptr);
    if (Ptr != nullptr) {
      EXPECT_TRUE(scudo::SharedArenaPool::getInstance().isArenaAddr(
          reinterpret_cast<scudo::uptr>(Ptr)));
    }
    return Ptr;
  }

  std::unique_ptr<LargeAllocator> Allocator =
      std::make_unique<LargeAllocator>();
  scudo::Options Options =
      getOptionsForConfig<scudo::SecondaryConfig<Config>>();
};

TEST_F(SharedArenaDelegationTest, DelegatedThreadedAllocFree) {
  constexpr scudo::uptr Size = (1U << 16) + 123;
  constexpr scudo::uptr Iterations = 32;
  constexpr scudo::uptr ThreadCount = 8;

  std::atomic<scudo::u32> ArenaHits{0};
  const SharedArenaTotals Before = getSharedArenaTotals();

  std::vector<void *> AllPtrs;
  AllPtrs.reserve(ThreadCount * Iterations);

  std::thread Threads[ThreadCount];
  for (scudo::uptr I = 0; I < ThreadCount; ++I) {
    Threads[I] = std::thread([&, I] {
      std::vector<void *> Local;
      Local.reserve(Iterations);
      for (scudo::uptr J = 0; J < Iterations; ++J) {
        void *Ptr = Allocator->allocate(Options, Size + (I + J) % 64);
        EXPECT_NE(Ptr, nullptr);
        if (Ptr == nullptr)
          continue;
        EXPECT_TRUE(scudo::SharedArenaPool::getInstance().isArenaAddr(
            reinterpret_cast<scudo::uptr>(Ptr)));
        ArenaHits.fetch_add(1, std::memory_order_relaxed);
        memset(Ptr, static_cast<int>(I + J), Size + (I + J) % 64);
        Local.push_back(Ptr);
      }
      {
        // 合并到全局数组，稍后统一检查和释放。
        static std::mutex M;
        std::lock_guard<std::mutex> Lock(M);
        AllPtrs.insert(AllPtrs.end(), Local.begin(), Local.end());
      }
    });
  }

  for (auto &Thread : Threads)
    Thread.join();

  // 检查同一时刻仍然存活的分配地址互不重复。
  EXPECT_EQ(ArenaHits.load(std::memory_order_relaxed),
            ThreadCount * Iterations);
  ASSERT_EQ(AllPtrs.size(),
            static_cast<size_t>(ThreadCount * Iterations));

  std::vector<scudo::uptr> Sorted;
  Sorted.reserve(AllPtrs.size());
  for (void *P : AllPtrs)
    Sorted.push_back(reinterpret_cast<scudo::uptr>(P));
  std::sort(Sorted.begin(), Sorted.end());
  auto NewEnd = std::unique(Sorted.begin(), Sorted.end());
  EXPECT_EQ(static_cast<size_t>(std::distance(Sorted.begin(), NewEnd)),
            Sorted.size());

  // 统一释放。
  for (void *P : AllPtrs)
    Allocator->deallocate(Options, P);

  const SharedArenaTotals After = getSharedArenaTotals();
  EXPECT_GT(After.RetrievedBytes, Before.RetrievedBytes);
  EXPECT_GT(After.DonatedBytes, Before.DonatedBytes);
  EXPECT_GT(After.FreeCount, 0U);
}

TEST_F(SharedArenaDelegationTest, DelegatedProcessAllocFree) {
  const scudo::uptr PageSize = scudo::getPageSizeCached();
  const scudo::uptr Size = 31 * PageSize - 128;

  const SharedArenaTotals Before = getSharedArenaTotals();
  void *ParentPtr = allocate(Size);
  ASSERT_NE(ParentPtr, nullptr);
  memset(ParentPtr, 0x5A, Size);
  Allocator->deallocate(Options, ParentPtr);

  const SharedArenaTotals AfterParentFree = getSharedArenaTotals();
  EXPECT_GT(AfterParentFree.DonatedBytes, Before.DonatedBytes);

  int PipeFds[2];
  ASSERT_EQ(pipe(PipeFds), 0);

  const pid_t Pid = fork();
  ASSERT_GE(Pid, 0) << strerror(errno);

  if (Pid == 0) {
    close(PipeFds[0]);
    scudo::setSharedArenaForceForTesting(true);

    LargeAllocator ChildAllocator;
    ChildAllocator.init(nullptr);
    void *ChildPtr = ChildAllocator.allocate(Options, Size);
    const scudo::uptr ChildValue = reinterpret_cast<scudo::uptr>(ChildPtr);
    const bool IsArenaPtr =
        ChildPtr != nullptr &&
        scudo::SharedArenaPool::getInstance().isArenaAddr(ChildValue);
    const bool ReusedSameVa =
        ChildValue == reinterpret_cast<scudo::uptr>(ParentPtr);
    (void)!write(PipeFds[1], &ChildValue, sizeof(ChildValue));
    if (ChildPtr != nullptr) {
      memset(ChildPtr, 0xA5, Size);
      ChildAllocator.deallocate(Options, ChildPtr);
    }
    close(PipeFds[1]);
    _exit(IsArenaPtr && ReusedSameVa ? 0 : 1);
  }

  close(PipeFds[1]);
  scudo::uptr ChildValue = 0;
  ASSERT_EQ(read(PipeFds[0], &ChildValue, sizeof(ChildValue)),
            static_cast<ssize_t>(sizeof(ChildValue)));
  close(PipeFds[0]);

  int Status = 0;
  ASSERT_EQ(waitpid(Pid, &Status, 0), Pid);
  EXPECT_FALSE(WIFSIGNALED(Status));
  EXPECT_EQ(WEXITSTATUS(Status), 0);
  EXPECT_EQ(ChildValue, reinterpret_cast<scudo::uptr>(ParentPtr));

  const SharedArenaTotals AfterChild = getSharedArenaTotals();
  EXPECT_GT(AfterChild.RetrievedBytes, AfterParentFree.RetrievedBytes);
  EXPECT_GT(AfterChild.DonatedBytes, AfterParentFree.DonatedBytes);
}

TEST_F(SharedArenaDelegationTest, DelegatedSiblingProcessesAllocFree) {
  const scudo::uptr PageSize = scudo::getPageSizeCached();
  const scudo::uptr Size = 19 * PageSize - 256;

  // 预热一次，使 Arena 中有可复用的块。
  void *SeedPtr = allocate(Size);
  ASSERT_NE(SeedPtr, nullptr);
  Allocator->deallocate(Options, SeedPtr);
  const scudo::uptr SeedVa = reinterpret_cast<scudo::uptr>(SeedPtr);

  // 子进程 A：从 Arena 中分配一个块并把 VA 写回父进程。
  int PipeA[2];
  ASSERT_EQ(pipe(PipeA), 0);

  pid_t PidA = fork();
  ASSERT_GE(PidA, 0) << strerror(errno);

  if (PidA == 0) {
    close(PipeA[0]);
    scudo::setSharedArenaForceForTesting(true);

    LargeAllocator ChildAllocator;
    ChildAllocator.init(nullptr);

    void *P = ChildAllocator.allocate(Options, Size);
    const scudo::uptr Va = reinterpret_cast<scudo::uptr>(P);
    const bool IsArenaPtr =
        P != nullptr &&
        scudo::SharedArenaPool::getInstance().isArenaAddr(Va);

    (void)!write(PipeA[1], &Va, sizeof(Va));

    if (P != nullptr) {
      memset(P, 0x3C, Size);
      ChildAllocator.deallocate(Options, P);
    }

    close(PipeA[1]);
    _exit(IsArenaPtr ? 0 : 1);
  }

  close(PipeA[1]);
  scudo::uptr VaA = 0;
  ASSERT_EQ(read(PipeA[0], &VaA, sizeof(VaA)),
            static_cast<ssize_t>(sizeof(VaA)));
  close(PipeA[0]);

  int StatusA = 0;
  ASSERT_EQ(waitpid(PidA, &StatusA, 0), PidA);
  EXPECT_FALSE(WIFSIGNALED(StatusA));
  EXPECT_EQ(WEXITSTATUS(StatusA), 0);
  // 子进程 A 应该复用到 Arena 地址空间。
  EXPECT_TRUE(scudo::SharedArenaPool::getInstance().isArenaAddr(VaA));

  // 子进程 B：在不知道 A 进程内部状态的前提下，再次从 Arena 中分配，
  // 期望拿到与 A 相同的 VA，实现跨进程委派复用。
  int PipeB[2];
  ASSERT_EQ(pipe(PipeB), 0);

  pid_t PidB = fork();
  ASSERT_GE(PidB, 0) << strerror(errno);

  if (PidB == 0) {
    close(PipeB[1]);
    scudo::setSharedArenaForceForTesting(true);

    scudo::uptr ExpectVa = 0;
    if (read(PipeB[0], &ExpectVa, sizeof(ExpectVa)) !=
        static_cast<ssize_t>(sizeof(ExpectVa)))
      _exit(1);

    LargeAllocator ChildAllocator;
    ChildAllocator.init(nullptr);
    void *P = ChildAllocator.allocate(Options, Size);
    const scudo::uptr VaB = reinterpret_cast<scudo::uptr>(P);

    const bool IsArenaPtr =
        P != nullptr &&
        scudo::SharedArenaPool::getInstance().isArenaAddr(VaB);
    const bool ReusedSiblingVa = VaB == ExpectVa;

    if (P != nullptr) {
      memset(P, 0x4D, Size);
      ChildAllocator.deallocate(Options, P);
    }

    close(PipeB[0]);
    _exit(IsArenaPtr && ReusedSiblingVa ? 0 : 1);
  }

  close(PipeB[0]);
  ASSERT_EQ(write(PipeB[1], &VaA, sizeof(VaA)),
            static_cast<ssize_t>(sizeof(VaA)));
  close(PipeB[1]);

  int StatusB = 0;
  ASSERT_EQ(waitpid(PidB, &StatusB, 0), PidB);
  EXPECT_FALSE(WIFSIGNALED(StatusB));
  EXPECT_EQ(WEXITSTATUS(StatusB), 0);

  // 验证 sibling 进程场景下，Arena 的全局统计也有明显增长。
  const SharedArenaTotals After = getSharedArenaTotals();
  EXPECT_GT(After.RetrievedBytes, 0U);
  EXPECT_GT(After.DonatedBytes, 0U);
}

static const char *getSelfExePath() {
#if SCUDO_LINUX
  static char Buf[PATH_MAX];
  const ssize_t N = readlink("/proc/self/exe", Buf, sizeof(Buf) - 1);
  if (N <= 0)
    return nullptr;
  Buf[N] = '\0';
  return Buf;
#else
  return nullptr;
#endif
}

TEST_F(SharedArenaDelegationTest,
       DelegatedIndependentProcessAllocFree_ExecAttachReuse) {
#if SCUDO_LINUX
  const scudo::uptr PageSize = scudo::getPageSizeCached();
  const scudo::uptr Size = 31 * PageSize - 128;

  const SharedArenaTotals Before = getSharedArenaTotals();
  void *ParentPtr = allocate(Size);
  ASSERT_NE(ParentPtr, nullptr);
  memset(ParentPtr, 0x5A, Size);
  Allocator->deallocate(Options, ParentPtr);

  const scudo::uptr ExpectedVa = reinterpret_cast<scudo::uptr>(ParentPtr);
  const SharedArenaTotals AfterParentFree = getSharedArenaTotals();
  EXPECT_GT(AfterParentFree.DonatedBytes, Before.DonatedBytes);

  // Pipe for passing the expected VA to the execed child.
  int PipeFds[2];
  ASSERT_EQ(pipe(PipeFds), 0);

  pid_t Pid = fork();
  ASSERT_GE(Pid, 0) << strerror(errno);

  if (Pid == 0) {
    // Child process: exec into the same test binary, but run only the child
    // helper test and enable SHARED_ARENA_ATTACH=1.
    close(PipeFds[1]); // close write end
    // Duplicate read end to a stable fd so the child test can read it.
    ASSERT_EQ(dup2(PipeFds[0], 3), 3);
    close(PipeFds[0]);

    setenv("SCUDO_SHARED_ARENA_ATTACH", "1", 1);
    setenv("SCUDO_SHARED_ARENA_CHILD_MODE", "1", 1);

    const char *Self = getSelfExePath();
    ASSERT_NE(Self, nullptr);

    // Run only the helper test.
    execl(Self, Self, "--gtest_filter=SharedArenaDelegationTest."
                      "DelegatedIndependentProcessChildAllocReuse",
          static_cast<char *>(nullptr));
    _exit(1);
  }

  // Parent process: write the expected VA to the child then wait.
  close(PipeFds[0]);
  ASSERT_EQ(write(PipeFds[1], &ExpectedVa, sizeof(ExpectedVa)),
            static_cast<ssize_t>(sizeof(ExpectedVa)));
  close(PipeFds[1]);

  int Status = 0;
  ASSERT_EQ(waitpid(Pid, &Status, 0), Pid);
  EXPECT_FALSE(WIFSIGNALED(Status));
  EXPECT_EQ(WEXITSTATUS(Status), 0);
  const SharedArenaTotals AfterChild = getSharedArenaTotals();
  EXPECT_GT(AfterChild.RetrievedBytes, AfterParentFree.RetrievedBytes);
#endif
}

TEST_F(SharedArenaDelegationTest,
       DelegatedIndependentProcessChildAllocReuse) {
#if SCUDO_LINUX
  const char *Mode = getenv("SCUDO_SHARED_ARENA_CHILD_MODE");
  if (!Mode || Mode[0] == '\0' || Mode[0] == '0')
    TEST_SKIP("Child helper only; run via exec from parent test");

  // Helper test for execed child in the parent test above.
  scudo::uptr ExpectedVa = 0;
  const ssize_t N = read(3, &ExpectedVa, sizeof(ExpectedVa));
  ASSERT_EQ(N, static_cast<ssize_t>(sizeof(ExpectedVa)));

  const scudo::uptr PageSize = scudo::getPageSizeCached();
  const scudo::uptr Size = 31 * PageSize - 128;

  const SharedArenaTotals Before = getSharedArenaTotals();

  LargeAllocator ChildAllocator;
  ChildAllocator.init(nullptr);
  void *ChildPtr = ChildAllocator.allocate(Options, Size);
  const scudo::uptr ChildVa = reinterpret_cast<scudo::uptr>(ChildPtr);
  const bool IsArenaPtr = ChildPtr != nullptr &&
                           scudo::SharedArenaPool::getInstance().isArenaAddr(
                               ChildVa);
  EXPECT_TRUE(IsArenaPtr);
  EXPECT_EQ(ChildVa, ExpectedVa);

  if (ChildPtr != nullptr)
    ChildAllocator.deallocate(Options, ChildPtr);

  const SharedArenaTotals After = getSharedArenaTotals();
  EXPECT_GT(After.RetrievedBytes, Before.RetrievedBytes);
#endif
}
#endif // SCUDO_LINUX

TEST(ScudoSecondaryTest, SecondaryBasic) {
  testSecondaryBasic<NoCacheConfig>();
  testSecondaryBasic<scudo::DefaultConfig>();
  testSecondaryBasic<TestConfig>();
}

struct MapAllocatorTest : public Test {
  using Config = scudo::DefaultConfig;
  using LargeAllocator = scudo::MapAllocator<scudo::SecondaryConfig<Config>>;

  void SetUp() override { Allocator->init(nullptr); }

  void TearDown() override { Allocator->unmapTestOnly(); }

  std::unique_ptr<LargeAllocator> Allocator =
      std::make_unique<LargeAllocator>();
  scudo::Options Options =
      getOptionsForConfig<scudo::SecondaryConfig<Config>>();
};

// This exercises a variety of combinations of size and alignment for the
// MapAllocator. The size computation done here mimic the ones done by the
// combined allocator.
TEST_F(MapAllocatorTest, SecondaryCombinations) {
  constexpr scudo::uptr MinAlign = FIRST_32_SECOND_64(8, 16);
  constexpr scudo::uptr HeaderSize = scudo::roundUp(8, MinAlign);
  for (scudo::uptr SizeLog = 0; SizeLog <= 20; SizeLog++) {
    for (scudo::uptr AlignLog = FIRST_32_SECOND_64(3, 4); AlignLog <= 16;
         AlignLog++) {
      const scudo::uptr Align = 1U << AlignLog;
      for (scudo::sptr Delta = -128; Delta <= 128; Delta += 8) {
        if ((1LL << SizeLog) + Delta <= 0)
          continue;
        const scudo::uptr UserSize = scudo::roundUp(
            static_cast<scudo::uptr>((1LL << SizeLog) + Delta), MinAlign);
        const scudo::uptr Size =
            HeaderSize + UserSize + (Align > MinAlign ? Align - HeaderSize : 0);
        void *P = Allocator->allocate(Options, Size, Align);
        EXPECT_NE(P, nullptr);
        void *AlignedP = reinterpret_cast<void *>(
            scudo::roundUp(reinterpret_cast<scudo::uptr>(P), Align));
        memset(AlignedP, 0xff, UserSize);
        Allocator->deallocate(Options, P);
      }
    }
  }
  scudo::ScopedString Str;
  Allocator->getStats(&Str);
  Str.output();
}

TEST_F(MapAllocatorTest, SecondaryIterate) {
  std::vector<void *> V;
  const scudo::uptr PageSize = scudo::getPageSizeCached();
  for (scudo::uptr I = 0; I < 32U; I++)
    V.push_back(Allocator->allocate(
        Options, (static_cast<scudo::uptr>(std::rand()) % 16U) * PageSize));
  auto Lambda = [&V](scudo::uptr Block) {
    EXPECT_NE(std::find(V.begin(), V.end(), reinterpret_cast<void *>(Block)),
              V.end());
  };
  Allocator->disable();
  Allocator->iterateOverBlocks(Lambda);
  Allocator->enable();
  while (!V.empty()) {
    Allocator->deallocate(Options, V.back());
    V.pop_back();
  }
  scudo::ScopedString Str;
  Allocator->getStats(&Str);
  Str.output();
}

TEST_F(MapAllocatorTest, SecondaryCacheOptions) {
  if (!Allocator->canCache(0U))
    TEST_SKIP("Secondary Cache disabled");

  // Attempt to set a maximum number of entries higher than the array size.
  EXPECT_TRUE(Allocator->setOption(scudo::Option::MaxCacheEntriesCount, 4096U));

  // Attempt to set an invalid (negative) number of entries
  EXPECT_FALSE(Allocator->setOption(scudo::Option::MaxCacheEntriesCount, -1));

  // Various valid combinations.
  EXPECT_TRUE(Allocator->setOption(scudo::Option::MaxCacheEntriesCount, 4U));
  EXPECT_TRUE(
      Allocator->setOption(scudo::Option::MaxCacheEntrySize, 1UL << 20));
  EXPECT_TRUE(Allocator->canCache(1UL << 18));
  EXPECT_TRUE(
      Allocator->setOption(scudo::Option::MaxCacheEntrySize, 1UL << 17));
  EXPECT_FALSE(Allocator->canCache(1UL << 18));
  EXPECT_TRUE(Allocator->canCache(1UL << 16));
  EXPECT_TRUE(Allocator->setOption(scudo::Option::MaxCacheEntriesCount, 0U));
  EXPECT_FALSE(Allocator->canCache(1UL << 16));
  EXPECT_TRUE(Allocator->setOption(scudo::Option::MaxCacheEntriesCount, 4U));
  EXPECT_TRUE(
      Allocator->setOption(scudo::Option::MaxCacheEntrySize, 1UL << 20));
  EXPECT_TRUE(Allocator->canCache(1UL << 16));
}

struct MapAllocatorWithReleaseTest : public MapAllocatorTest {
  void SetUp() override { Allocator->init(nullptr, /*ReleaseToOsInterval=*/0); }

  void performAllocations() {
    std::vector<void *> V;
    const scudo::uptr PageSize = scudo::getPageSizeCached();
    {
      std::unique_lock<std::mutex> Lock(Mutex);
      while (!Ready)
        Cv.wait(Lock);
    }
    for (scudo::uptr I = 0; I < 128U; I++) {
      // Deallocate 75% of the blocks.
      const bool Deallocate = (std::rand() & 3) != 0;
      void *P = Allocator->allocate(
          Options, (static_cast<scudo::uptr>(std::rand()) % 16U) * PageSize);
      if (Deallocate)
        Allocator->deallocate(Options, P);
      else
        V.push_back(P);
    }
    while (!V.empty()) {
      Allocator->deallocate(Options, V.back());
      V.pop_back();
    }
  }

  std::mutex Mutex;
  std::condition_variable Cv;
  bool Ready = false;
};

TEST_F(MapAllocatorWithReleaseTest, SecondaryThreadsRace) {
  std::thread Threads[16];
  for (scudo::uptr I = 0; I < ARRAY_SIZE(Threads); I++)
    Threads[I] =
        std::thread(&MapAllocatorWithReleaseTest::performAllocations, this);
  {
    std::unique_lock<std::mutex> Lock(Mutex);
    Ready = true;
    Cv.notify_all();
  }
  for (auto &T : Threads)
    T.join();
  scudo::ScopedString Str;
  Allocator->getStats(&Str);
  Str.output();
}

struct MapAllocatorCacheTest : public Test {
  static constexpr scudo::u32 UnmappedMarker = 0xDEADBEEF;

  static void testUnmapCallback(scudo::MemMapT &MemMap) {
    scudo::u32 *Ptr = reinterpret_cast<scudo::u32 *>(MemMap.getBase());
    *Ptr = UnmappedMarker;
  }

  using SecondaryConfig = scudo::SecondaryConfig<TestConfig>;
  using CacheConfig = SecondaryConfig::CacheConfig;
  using CacheT = scudo::MapAllocatorCache<CacheConfig, testUnmapCallback>;

  std::unique_ptr<CacheT> Cache = std::make_unique<CacheT>();

  const scudo::uptr PageSize = scudo::getPageSizeCached();
  // The current test allocation size is set to the maximum
  // cache entry size
  static constexpr scudo::uptr TestAllocSize =
      CacheConfig::getDefaultMaxEntrySize();

  scudo::Options Options = getOptionsForConfig<SecondaryConfig>();

  void SetUp() override { Cache->init(/*ReleaseToOsInterval=*/-1); }

  void TearDown() override { Cache->unmapTestOnly(); }

  scudo::MemMapT allocate(scudo::uptr Size) {
    scudo::uptr MapSize = scudo::roundUp(Size, PageSize);
    scudo::ReservedMemoryT ReservedMemory;
    CHECK(ReservedMemory.create(0U, MapSize, nullptr, MAP_ALLOWNOMEM));

    scudo::MemMapT MemMap = ReservedMemory.dispatch(
        ReservedMemory.getBase(), ReservedMemory.getCapacity());
    MemMap.remap(MemMap.getBase(), MemMap.getCapacity(), "scudo:test",
                 MAP_RESIZABLE | MAP_ALLOWNOMEM);
    return MemMap;
  }

  void fillCacheWithSameSizeBlocks(std::vector<scudo::MemMapT> &MemMaps,
                                   scudo::uptr NumEntries, scudo::uptr Size) {
    for (scudo::uptr I = 0; I < NumEntries; I++) {
      MemMaps.emplace_back(allocate(Size));
      auto &MemMap = MemMaps[I];
      Cache->store(Options, MemMap.getBase(), MemMap.getCapacity(),
                   MemMap.getBase(), MemMap);
    }
  }
};

TEST_F(MapAllocatorCacheTest, CacheOrder) {
  std::vector<scudo::MemMapT> MemMaps;
  Cache->setOption(scudo::Option::MaxCacheEntriesCount,
                   CacheConfig::getEntriesArraySize());

  fillCacheWithSameSizeBlocks(MemMaps, CacheConfig::getEntriesArraySize(),
                              TestAllocSize);

  // Retrieval order should be the inverse of insertion order
  for (scudo::uptr I = CacheConfig::getEntriesArraySize(); I > 0; I--) {
    scudo::uptr EntryHeaderPos;
    scudo::CachedBlock Entry =
        Cache->retrieve(0, TestAllocSize, PageSize, 0, EntryHeaderPos);
    EXPECT_EQ(Entry.MemMap.getBase(), MemMaps[I - 1].getBase());
  }

  // Clean up MemMaps
  for (auto &MemMap : MemMaps)
    MemMap.unmap();
}

TEST_F(MapAllocatorCacheTest, PartialChunkHeuristicRetrievalTest) {
  const scudo::uptr FragmentedPages =
      1 + scudo::CachedBlock::MaxReleasedCachePages;
  scudo::uptr EntryHeaderPos;
  scudo::CachedBlock Entry;
  scudo::MemMapT MemMap = allocate(PageSize + FragmentedPages * PageSize);
  Cache->store(Options, MemMap.getBase(), MemMap.getCapacity(),
               MemMap.getBase(), MemMap);

  // FragmentedPages > MaxAllowedFragmentedPages so PageSize
  // cannot be retrieved from the cache
  Entry = Cache->retrieve(/*MaxAllowedFragmentedPages=*/0, PageSize, PageSize,
                          0, EntryHeaderPos);
  EXPECT_FALSE(Entry.isValid());

  // FragmentedPages == MaxAllowedFragmentedPages so PageSize
  // can be retrieved from the cache
  Entry =
      Cache->retrieve(FragmentedPages, PageSize, PageSize, 0, EntryHeaderPos);
  EXPECT_TRUE(Entry.isValid());

  MemMap.unmap();
}

TEST_F(MapAllocatorCacheTest, MemoryLeakTest) {
  std::vector<scudo::MemMapT> MemMaps;
  // Fill the cache above MaxEntriesCount to force an eviction
  // The first cache entry should be evicted (because it is the oldest)
  // due to the maximum number of entries being reached
  fillCacheWithSameSizeBlocks(
      MemMaps, CacheConfig::getDefaultMaxEntriesCount() + 1, TestAllocSize);

  std::vector<scudo::CachedBlock> RetrievedEntries;

  // First MemMap should be evicted from cache because it was the first
  // inserted into the cache
  for (scudo::uptr I = CacheConfig::getDefaultMaxEntriesCount(); I > 0; I--) {
    scudo::uptr EntryHeaderPos;
    RetrievedEntries.push_back(
        Cache->retrieve(0, TestAllocSize, PageSize, 0, EntryHeaderPos));
    EXPECT_EQ(MemMaps[I].getBase(), RetrievedEntries.back().MemMap.getBase());
  }

  // Evicted entry should be marked due to unmap callback
  EXPECT_EQ(*reinterpret_cast<scudo::u32 *>(MemMaps[0].getBase()),
            UnmappedMarker);

  // Clean up MemMaps
  for (auto &MemMap : MemMaps)
    MemMap.unmap();
}
