//===-- shared_arena_latency_bench.cpp --------------------------*- C++ -*-===//
//
// Latency benchmark: delegated SharedArena path vs traditional Secondary path.
// See TEST.md. Intended for Linux/Android (e.g. adb shell).
//
//===----------------------------------------------------------------------===//

#include "allocator_config.h"
#include "allocator_config_wrapper.h"
#include "secondary.h"
#include "shared_arena.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits.h>
#include <pthread.h>
#include <thread>
#include <vector>

#if SCUDO_LINUX
#include <sched.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using namespace scudo;

template <typename Config> static Options getOptionsForConfig() {
  if (!Config::getMaySupportMemoryTagging() ||
      !archSupportsMemoryTagging() || !systemSupportsMemoryTagging())
    return {};
  AtomicOptions AO;
  AO.set(OptionBit::UseMemoryTagging);
  return AO.load();
}

using BenchConfig = DefaultConfig;
using LargeAllocator = MapAllocator<SecondaryConfig<BenchConfig>>;

static uint64_t nowNs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

static bool pinSelfToCpu(int Cpu) {
#if SCUDO_LINUX
  cpu_set_t Set;
  CPU_ZERO(&Set);
  CPU_SET(Cpu, &Set);
  // Use thread-level affinity so process-level affinity (used by
  // SharedArenaPool::getNumberOfCPUs via sched_getaffinity) stays stable.
#if SCUDO_ANDROID
  // Android bionic does not expose pthread_setaffinity_np consistently across
  // API levels/toolchains; sched_setaffinity(0, ...) still pins the calling
  // thread on Linux/Android.
  return sched_setaffinity(0, sizeof(Set), &Set) == 0;
#else
  return pthread_setaffinity_np(pthread_self(), sizeof(Set), &Set) == 0;
#endif
#else
  (void)Cpu;
  return false;
#endif
}

static void printAffinity(const char *Label) {
#if SCUDO_LINUX
  cpu_set_t Set;
  CPU_ZERO(&Set);
  if (sched_getaffinity(0, sizeof(Set), &Set) != 0) {
    printf("%s affinity: <sched_getaffinity failed errno=%d>\n", Label, errno);
    return;
  }
  printf("%s affinity:", Label);
  const int Max = CPU_SETSIZE < 1024 ? CPU_SETSIZE : 1024;
  for (int C = 0; C < Max; ++C) {
    if (CPU_ISSET(C, &Set))
      printf(" %d", C);
  }
  printf("\n");
#else
  printf("%s affinity: N/A (non-Linux)\n", Label);
#endif
}

static int firstUsableArenaCpu(SharedArenaPool &Pool) {
#if SCUDO_LINUX
  if (!Pool.isReady() || Pool.getNumCores() == 0)
    return -1;
  cpu_set_t Allowed;
  CPU_ZERO(&Allowed);
  if (sched_getaffinity(0, sizeof(Allowed), &Allowed) != 0)
    return -1;
  for (u32 Cpu = 0; Cpu < Pool.getNumCores(); ++Cpu) {
    if (CPU_ISSET(static_cast<int>(Cpu), &Allowed))
      return static_cast<int>(Cpu);
  }
#endif
  return -1;
}

struct SampleStats {
  uint64_t N = 0;
  double SumNs = 0;
  uint64_t MinNs = UINT64_MAX;
  uint64_t MaxNs = 0;
  uint64_t P50 = 0, P95 = 0, P99 = 0;
};

static void computeStats(std::vector<uint64_t> &Samples, SampleStats &Out) {
  Out = {};
  if (Samples.empty())
    return;
  Out.N = Samples.size();
  for (uint64_t V : Samples) {
    Out.SumNs += static_cast<double>(V);
    if (V < Out.MinNs)
      Out.MinNs = V;
    if (V > Out.MaxNs)
      Out.MaxNs = V;
  }
  std::sort(Samples.begin(), Samples.end());
  auto at = [&](double P) -> uint64_t {
    if (Samples.empty())
      return 0;
    size_t Idx = static_cast<size_t>(
        (P / 100.0) * static_cast<double>(Samples.size() - 1));
    return Samples[Idx];
  };
  Out.P50 = at(50);
  Out.P95 = at(95);
  Out.P99 = at(99);
}

static void touchAllocatedPages(void *Ptr, uptr Size, uptr PageSize,
                                  unsigned char Pattern) {
  if (!Ptr || Size == 0 || PageSize == 0)
    return;
  auto *Bytes = reinterpret_cast<unsigned char *>(Ptr);
  // Touch one byte per page to force page commitment and model "application
  // use" cost that occurs before release.
  for (uptr Off = 0; Off + PageSize <= Size; Off += PageSize)
    Bytes[Off] = Pattern;
  Bytes[Size - 1] = Pattern;
}

static void runBenchRound(LargeAllocator &Alloc, Options &Opt, bool Delegated,
                          uptr Size, u32 Warmup, u64 Iterations,
                          u64 HoldUs, bool TouchPages, const char *Label,
                          std::vector<uint64_t> &OutSamples) {
  if (Delegated)
    setSharedArenaForceForTesting(true);
  else
    clearSharedArenaForceForTesting();

  OutSamples.clear();
  OutSamples.reserve(static_cast<size_t>(Iterations) * 3U);
  const uptr PageSize = getPageSizeCached();
  constexpr unsigned char kPattern = 0x5A;

  for (u32 I = 0; I < Warmup; ++I) {
    void *P = Alloc.allocate(Opt, Size);
    if (!P) {
      fprintf(stderr, "%s warmup alloc failed size=%zu iter=%u\n", Label,
              static_cast<size_t>(Size), I);
      return;
    }
    if (Delegated && !SharedArenaPool::getInstance().isArenaAddr(
                        reinterpret_cast<uptr>(P)))
      fprintf(stderr,
              "%s warmup: ptr not in arena (delegated path may have fallen "
              "back)\n",
              Label);
    if (TouchPages)
      touchAllocatedPages(P, Size, PageSize, kPattern);
    if (HoldUs != 0)
      std::this_thread::sleep_for(std::chrono::microseconds(HoldUs));
    Alloc.deallocate(Opt, P);
  }

  for (u64 I = 0; I < Iterations; ++I) {
    const uint64_t T0 = nowNs();
    void *P = Alloc.allocate(Opt, Size);
    const uint64_t T1 = nowNs();
    if (!P) {
      fprintf(stderr, "%s alloc failed at iter %" PRIu64 "\n", Label, I);
      return;
    }
    if (TouchPages)
      touchAllocatedPages(P, Size, PageSize, kPattern);
    if (HoldUs != 0)
      std::this_thread::sleep_for(std::chrono::microseconds(HoldUs));

    const uint64_t T2 = nowNs(); // end of "hold" (touch + use window)
    Alloc.deallocate(Opt, P);
    const uint64_t T3 = nowNs();

    OutSamples.push_back(T1 - T0); // alloc_only
    OutSamples.push_back(T2 - T1); // hold_only
    OutSamples.push_back(T3 - T2); // dealloc_only
  }
}

static void printStats(const char *Title, SampleStats &S) {
  if (S.N == 0) {
    printf("\t%-12s\tno samples\n", Title);
    return;
  }
  printf("\t%-12s\tmean=%-10.2f p50=%-8" PRIu64 " p95=%-8" PRIu64 " p99=%-8"
         PRIu64 "\n",
         Title, S.SumNs / static_cast<double>(S.N), S.P50, S.P95, S.P99);
}

static double meanNs(const SampleStats &S) {
  if (S.N == 0)
    return 0.0;
  return S.SumNs / static_cast<double>(S.N);
}

static void printStatsWithDelta(const char *Title, SampleStats &S,
                                const SampleStats *Baseline) {
  if (S.N == 0) {
    printf("\t%-12s\tno samples\n", Title);
    return;
  }

  const double Mean = meanNs(S);
  printf("\t%-12s\tmean=%-10.2f", Title, Mean);
  if (Baseline != nullptr && Baseline->N != 0) {
    const double BaseMean = meanNs(*Baseline);
    if (BaseMean > 0.0) {
      // Output delegated speedup as a multiplicative factor.
      // Speedup = BaseMean / Mean; delegated faster => >1x.
      const double Speedup = BaseMean / Mean;
      printf("(%-5.2fx)", Speedup);  // 左对齐
    }
  }
  printf(" p50=%-8" PRIu64 " p95=%-8" PRIu64 " p99=%-8" PRIu64 "\n",
         S.P50, S.P95, S.P99);
}

static const char *sizeUnit(uptr Size, double &Value) {
  if (Size >= (1UL << 20)) {
    Value = static_cast<double>(Size) / static_cast<double>(1UL << 20);
    return "MiB";
  }
  if (Size >= (1UL << 10)) {
    Value = static_cast<double>(Size) / static_cast<double>(1UL << 10);
    return "KiB";
  }
  Value = static_cast<double>(Size);
  return "B";
}

struct ThreadCtx {
  LargeAllocator *Alloc = nullptr;
  Options *Opt = nullptr;
  bool Delegated = false;
  uptr Size = 0;
  u32 Warmup = 0;
  u64 Iterations = 0;
  u64 HoldUs = 0;
  bool TouchPages = true;
  int Cpu = -1;
  std::vector<uint64_t> Samples;
  int Err = 0;
};

struct BenchSummary {
  SampleStats Alloc;
  SampleStats Hold;
  SampleStats Dealloc;
  SampleStats Round;
};

static void computeSummaryFromSamples(const std::vector<uint64_t> &Samples,
                                       BenchSummary &Out) {
  Out = {};
  if (Samples.empty())
    return;
  // Samples stores: alloc_time, hold_time, dealloc_time,
  //                  alloc_time, hold_time, dealloc_time, ...
  std::vector<uint64_t> AllocSamples;
  std::vector<uint64_t> HoldSamples;
  std::vector<uint64_t> DeallocSamples;
  std::vector<uint64_t> RoundSamples;
  AllocSamples.reserve(Samples.size() / 3 + 1);
  HoldSamples.reserve(Samples.size() / 3 + 1);
  DeallocSamples.reserve(Samples.size() / 3 + 1);
  RoundSamples.reserve(Samples.size() / 3 + 1);
  for (size_t I = 0; I + 2 < Samples.size(); I += 3) {
    AllocSamples.push_back(Samples[I]);
    HoldSamples.push_back(Samples[I + 1]);
    DeallocSamples.push_back(Samples[I + 2]);
    RoundSamples.push_back(Samples[I] + Samples[I + 1] + Samples[I + 2]);
  }
  computeStats(AllocSamples, Out.Alloc);
  computeStats(HoldSamples, Out.Hold);
  computeStats(DeallocSamples, Out.Dealloc);
  computeStats(RoundSamples, Out.Round);
}

struct ForkResultMsg {
  uint64_t Ok = 0;
  SampleStats Alloc;
  SampleStats Hold;
  SampleStats Dealloc;
  SampleStats Round;
};

static bool readExact(int Fd, void *Buf, size_t Size) {
  size_t Off = 0;
  while (Off < Size) {
    const ssize_t N = read(Fd, reinterpret_cast<char *>(Buf) + Off,
                            Size - Off);
    if (N <= 0)
      return false;
    Off += static_cast<size_t>(N);
  }
  return true;
}

static bool writeExact(int Fd, const void *Buf, size_t Size) {
  size_t Off = 0;
  while (Off < Size) {
    const ssize_t N = write(Fd, reinterpret_cast<const char *>(Buf) + Off,
                             Size - Off);
    if (N <= 0)
      return false;
    Off += static_cast<size_t>(N);
  }
  return true;
}

static void warmupAlloc(LargeAllocator &Alloc, Options &Opt, bool Delegated,
                         uptr Size, u32 Warmup, u64 HoldUs, bool TouchPages,
                         const char *Label) {
  if (Delegated)
    setSharedArenaForceForTesting(true);
  else
    clearSharedArenaForceForTesting();

  const uptr PageSize = getPageSizeCached();
  constexpr unsigned char kPattern = 0x5A;

  for (u32 I = 0; I < Warmup; ++I) {
    void *P = Alloc.allocate(Opt, Size);
    if (!P) {
      fprintf(stderr, "%s warmup alloc failed size=%zu iter=%u\n", Label,
              static_cast<size_t>(Size), I);
      return;
    }
    if (Delegated && !SharedArenaPool::getInstance().isArenaAddr(
                          reinterpret_cast<uptr>(P))) {
      fprintf(stderr,
              "%s warmup: ptr not in arena (delegated may have fallen back)\n",
              Label);
    }
    if (TouchPages)
      touchAllocatedPages(P, Size, PageSize, kPattern);
    if (HoldUs != 0)
      std::this_thread::sleep_for(std::chrono::microseconds(HoldUs));
    Alloc.deallocate(Opt, P);
  }
}

static BenchSummary runCrossProcessFork(LargeAllocator &ParentAlloc, Options &Opt,
                                        uptr Size, u32 Warmup, u64 Iterations,
                                        bool ParentDelegated,
                                        bool ChildDelegated, int PinCpu,
                                        u64 HoldUs, bool TouchPages) {
  warmupAlloc(ParentAlloc, Opt, ParentDelegated, Size, Warmup, HoldUs,
               TouchPages, ParentDelegated ? "parent" : "parent(trad)");

  int Fds[2];
  if (pipe(Fds) != 0) {
    fprintf(stderr, "pipe() failed errno=%d\n", errno);
    return {};
  }

  const pid_t Pid = fork();
  if (Pid < 0) {
    fprintf(stderr, "fork() failed errno=%d\n", errno);
    close(Fds[0]);
    close(Fds[1]);
    return {};
  }

  if (Pid == 0) {
    // Child.
    close(Fds[0]);
    if (PinCpu >= 0)
      pinSelfToCpu(PinCpu);

    GlobalStats S;
    S.init();
    LargeAllocator ChildAlloc;
    ChildAlloc.init(&S);

    std::vector<uint64_t> Samples;
    runBenchRound(ChildAlloc, Opt, ChildDelegated, Size, Warmup, Iterations,
                  HoldUs, TouchPages, "child", Samples);

    ForkResultMsg Msg;
    Msg.Ok = 1;
    BenchSummary Summary;
    computeSummaryFromSamples(Samples, Summary);
    Msg.Alloc = Summary.Alloc;
    Msg.Hold = Summary.Hold;
    Msg.Dealloc = Summary.Dealloc;
    Msg.Round = Summary.Round;

    (void)writeExact(Fds[1], &Msg, sizeof(Msg));
    close(Fds[1]);
    _exit(0);
  }

  // Parent.
  close(Fds[1]);
  ForkResultMsg Msg;
  const bool ReadOk = readExact(Fds[0], &Msg, sizeof(Msg));
  close(Fds[0]);
  int Status = 0;
  (void)waitpid(Pid, &Status, 0);

  if (!ReadOk || Msg.Ok == 0)
    return {};

  BenchSummary Out;
  Out.Alloc = Msg.Alloc;
  Out.Hold = Msg.Hold;
  Out.Dealloc = Msg.Dealloc;
  Out.Round = Msg.Round;
  return Out;
}

static void threadMain(ThreadCtx *Ctx) {
  if (Ctx->Cpu >= 0 && !pinSelfToCpu(Ctx->Cpu)) {
    Ctx->Err = errno;
    return;
  }
  runBenchRound(*Ctx->Alloc, *Ctx->Opt, Ctx->Delegated, Ctx->Size, Ctx->Warmup,
                Ctx->Iterations, Ctx->HoldUs, Ctx->TouchPages, "thread",
                Ctx->Samples);
}

} // namespace

int main(int argc, char **argv) {
#if !SCUDO_LINUX
  printf("shared_arena_latency_bench: only supported on Linux/Android.\n");
  return 0;
#else
  const char *SelfExe = nullptr;
  u64 Iterations = 50000;
  u32 Warmup = 1000;
  u64 HoldUs = 0;
  u32 ThreadCount = 1;
  bool TouchPages = true;
  bool ProcessMode = false;
  bool IndependentProcessMode = false;
  u32 IndependentProcessCount = 4;

  // Internal role: donor/worker for independent-process mode.
  const char *Role = nullptr; // "donor" | "worker"
  bool RoleDelegated = false;
  bool RoleAttachEnv = false;
  bool RoleForceOffEnv = false;
  u32 RoleCpu = 0;
  int RoleOutFd = -1;
  uptr RoleSize = 0;
  // Default sizes: page-scale + typical secondary (see TEST.md)
  std::vector<uptr> Sizes;
  const uptr Page = getPageSizeCached();
  Sizes.push_back(31 * Page - 128);
  Sizes.push_back(64U << 10);
  Sizes.push_back(256U << 10);
  Sizes.push_back(1U << 20);

  for (int I = 1; I < argc; ++I) {
    if (!strcmp(argv[I], "--iterations") && I + 1 < argc) {
      Iterations = static_cast<u64>(strtoull(argv[++I], nullptr, 10));
    } else if (!strcmp(argv[I], "--warmup") && I + 1 < argc) {
      Warmup = static_cast<u32>(strtoul(argv[++I], nullptr, 10));
    } else if (!strcmp(argv[I], "--hold-us") && I + 1 < argc) {
      HoldUs = static_cast<u64>(strtoull(argv[++I], nullptr, 10));
    } else if (!strcmp(argv[I], "--touch-pages") && I + 1 < argc) {
      TouchPages = !strcmp(argv[++I], "1");
    } else if (!strcmp(argv[I], "--threads") && I + 1 < argc) {
      ThreadCount = static_cast<u32>(strtoul(argv[++I], nullptr, 10));
    } else if (!strcmp(argv[I], "--mode") && I + 1 < argc) {
      const char *M = argv[++I];
      ProcessMode = !strcmp(M, "process");
      IndependentProcessMode = !strcmp(M, "independent-process");
    } else if (!strcmp(argv[I], "--sizes") && I + 1 < argc) {
      Sizes.clear();
      const char *P = argv[++I];
      while (*P) {
        char *End = nullptr;
        unsigned long long V = strtoull(P, &End, 0);
        if (End == P)
          break;
        Sizes.push_back(static_cast<uptr>(V));
        if (*End != ',')
          break;
        P = End + 1;
      }
    } else if (!strcmp(argv[I], "--processes") && I + 1 < argc) {
      IndependentProcessCount = static_cast<u32>(strtoul(argv[++I], nullptr, 10));
    } else if (!strcmp(argv[I], "--role") && I + 1 < argc) {
      Role = argv[++I];
    } else if (!strcmp(argv[I], "--delegated") && I + 1 < argc) {
      RoleDelegated = !strcmp(argv[++I], "1");
    } else if (!strcmp(argv[I], "--attach-env") && I + 1 < argc) {
      RoleAttachEnv = !strcmp(argv[++I], "1");
    } else if (!strcmp(argv[I], "--force-off-env") && I + 1 < argc) {
      RoleForceOffEnv = !strcmp(argv[++I], "1");
    } else if (!strcmp(argv[I], "--cpu") && I + 1 < argc) {
      RoleCpu = static_cast<u32>(strtoul(argv[++I], nullptr, 10));
    } else if (!strcmp(argv[I], "--out-fd") && I + 1 < argc) {
      RoleOutFd = static_cast<int>(strtol(argv[++I], nullptr, 10));
    } else if (!strcmp(argv[I], "--size") && I + 1 < argc) {
      RoleSize = static_cast<uptr>(strtoull(argv[++I], nullptr, 0));
    } else if (!strcmp(argv[I], "--help") || !strcmp(argv[I], "-h")) {
      printf(
          "Usage: %s [options]\n"
          "  --iterations N   rounds per thread (default 50000)\n"
          "  --warmup N       warmup rounds (default 1000)\n"
          "  --hold-us U      touch+sleep microseconds between alloc and free (default 0)\n"
          "  --touch-pages 0|1 touch one byte per page (default 1)\n"
          "  --threads T      worker threads (default 1); each pinned to "
          "cpu 0..T-1\n"
          "  --mode thread|process|independent-process (default thread)\n"
          "  --processes P    used by independent-process mode (default 4)\n"
          "  --sizes a,b,c    allocation sizes in bytes (default: built-in "
          "set)\n",
          argv[0]);
      return 0;
    }
  }

  // Resolve our own executable path for exec() calls.
#if SCUDO_LINUX
  {
    static char Buf[PATH_MAX];
    const ssize_t N = readlink("/proc/self/exe", Buf, sizeof(Buf) - 1);
    if (N > 0) {
      Buf[N] = '\0';
      SelfExe = Buf;
    }
  }
#endif

  // Role path: donor/worker executed by independent-process mode.
  if (Role != nullptr) {
    // Set env before allocator initialization.
    if (RoleAttachEnv)
      setenv("SCUDO_SHARED_ARENA_ATTACH", "1", 1);
    else
      unsetenv("SCUDO_SHARED_ARENA_ATTACH");
    if (RoleForceOffEnv)
      setenv("SCUDO_SHARED_ARENA_FORCE", "0", 1);
    else
      unsetenv("SCUDO_SHARED_ARENA_FORCE");
    if (!sharedArenaTraceEnabled())
      unsetenv("SCUDO_SHARED_ARENA_TRACE");

    GlobalStats S;
    S.init();
    LargeAllocator Alloc;
    Alloc.init(&S);

    Options Opt = getOptionsForConfig<SecondaryConfig<BenchConfig>>();

      // Pin after Alloc.init so SharedArenaPool::init can compute NumCores from
      // the broader affinity mask (it uses sched_getaffinity(0,...)).
      // Pinning too early would shrink NumCores to 1 and make getCurrentArena()
      // reject CPUs >= NumCores.
      if (RoleCpu >= 0 && !pinSelfToCpu(static_cast<int>(RoleCpu))) {
        fprintf(stderr, "role %s pin cpu %u failed errno=%d\n", Role,
                RoleCpu, errno);
      }

    // Warmup-only donor: allocate/free Warmup blocks to seed freelist.
    if (!strcmp(Role, "donor")) {
      warmupAlloc(Alloc, Opt, RoleDelegated, RoleSize, Warmup, HoldUs,
                  TouchPages, "donor");
      _exit(0);
    }

    // Worker: measure alloc/free latency and send raw samples to parent.
    if (!strcmp(Role, "worker")) {
      if (RoleOutFd < 0) {
        fprintf(stderr, "worker role missing --out-fd\n");
        _exit(1);
      }
      std::vector<uint64_t> Samples;
      runBenchRound(Alloc, Opt, RoleDelegated, RoleSize, Warmup, Iterations,
                    HoldUs, TouchPages, "worker", Samples);
      uint64_t Len = static_cast<uint64_t>(Samples.size());
      (void)writeExact(RoleOutFd, &Len, sizeof(Len));
      if (Len > 0)
        (void)writeExact(RoleOutFd, Samples.data(), Len * sizeof(uint64_t));
      close(RoleOutFd);
      _exit(0);
    }

    _exit(1);
  }

  SharedArenaPool &Pool = SharedArenaPool::getInstance();
  GlobalStats S;
  S.init();
  LargeAllocator Alloc;
  Alloc.init(&S);

  if (!Pool.isReady()) {
    fprintf(stderr,
            "SharedArenaPool not ready — delegated path unavailable. "
            "(Check shm / dev/shm.)\n");
  }

  printf("SharedArena: ready=%d num_cores=%u page_size=%zu\n",
         Pool.isReady() ? 1 : 0, Pool.getNumCores(),
         static_cast<size_t>(Page));
  printAffinity("initial");

  const int PinCpu = firstUsableArenaCpu(Pool);
  if (PinCpu >= 0) {
    if (!IndependentProcessMode) {
      if (pinSelfToCpu(PinCpu))
        printf("pinned main thread to CPU %d\n", PinCpu);
      else
        fprintf(stderr, "pin(main->cpu %d) failed errno=%d\n", PinCpu,
                errno);
    } else {
      // In independent-process mode, keep a broader affinity mask for each
      // execed process so SharedArenaPool::init() sees a correct NumCores.
      printf("independent-process: skip pin main thread to keep affinity mask\n");
    }
  } else {
    fprintf(stderr, "could not pick arena CPU; results may be invalid.\n");
  }
  if (!IndependentProcessMode)
    printAffinity("after pin");

  Options Opt = getOptionsForConfig<SecondaryConfig<BenchConfig>>();

  for (uptr Size : Sizes) {
    double HumanSize = 0.0;
    const char *Unit = sizeUnit(Size, HumanSize);

    if (ProcessMode) {
      if (ThreadCount != 1)
        fprintf(stderr, "process mode ignores --threads, forcing threads=1\n");
      const int ChildPinCpu = PinCpu;

      BenchSummary Traditional =
          runCrossProcessFork(Alloc, Opt, Size, Warmup, Iterations,
                               /*ParentDelegated=*/false,
                               /*ChildDelegated=*/false, ChildPinCpu,
                               HoldUs, TouchPages);
      BenchSummary Delegated =
          runCrossProcessFork(Alloc, Opt, Size, Warmup, Iterations,
                               /*ParentDelegated=*/true,
                               /*ChildDelegated=*/true, ChildPinCpu,
                               HoldUs, TouchPages);

      printf("\n================== size=%.2f %s, mode=process(fork), unit=ns ==================\n",
             HumanSize, Unit);
      printf("-- delegated (SharedArena, parent->child) --\n");
      printStatsWithDelta("  alloc_only", Delegated.Alloc, &Traditional.Alloc);
      printStatsWithDelta("  hold_only", Delegated.Hold, &Traditional.Hold);
      printStatsWithDelta("  dealloc_only", Delegated.Dealloc,
                           &Traditional.Dealloc);
      printStatsWithDelta("  all", Delegated.Round,
                           &Traditional.Round);
      printf("-- traditional (cache/mmap, parent->child) --\n");
      printStats("  alloc_only", Traditional.Alloc);
      printStats("  hold_only", Traditional.Hold);
      printStats("  dealloc_only", Traditional.Dealloc);
      printStats("  all", Traditional.Round);
      continue;
    }

    if (IndependentProcessMode) {
      if (ThreadCount != 1)
        fprintf(stderr, "independent-process mode ignores --threads, forcing threads=1\n");
      if (IndependentProcessCount == 0)
        IndependentProcessCount = 1;

      if (!SelfExe) {
        fprintf(stderr, "could not resolve self exe path; --mode independent-process disabled.\n");
        return 1;
      }

      auto runIndependent =
          [&](bool Delegated) -> BenchSummary {
        // 1) Donor process(es) seed the Arena only in delegated run.
        // Worker CPU list is determined by the same modulo policy below:
        //   Cpu = (BaseCpu + P) % NumCores
        const u32 NumCores = Pool.getNumCores();
        const int BaseCpu = (PinCpu >= 0) ? PinCpu : 0;

#if SCUDO_ANDROID
        // Android 的 Arena backing 由当前父进程持有的 memfd / ASharedMemory fd
        // 提供；独立 exec 子进程只能 attach 到这些已继承的 fd，因此每轮先在父
        // 进程中 reset 共享状态，再让 donor / worker 全部走 attach-env=1。
        Pool.reset();
#endif

        std::vector<int> SeedCpus;
        SeedCpus.reserve(static_cast<size_t>(IndependentProcessCount));
        if (Delegated && NumCores > 0) {
          std::vector<char> Seen(NumCores, 0);
          for (u32 P = 0; P < IndependentProcessCount; ++P) {
            int Cpu = (BaseCpu + static_cast<int>(P)) %
                      static_cast<int>(NumCores);
            if (Cpu < 0)
              Cpu += static_cast<int>(NumCores);
            if (!Seen[static_cast<size_t>(Cpu)]) {
              Seen[static_cast<size_t>(Cpu)] = 1;
              SeedCpus.push_back(Cpu);
            }
          }
        }

        if (!Delegated || SeedCpus.empty()) {
          // Traditional run: a single donor is enough (it won't force arena).
          SeedCpus.push_back(BaseCpu);
          if (SeedCpus.back() < 0)
            SeedCpus.back() = 0;
        }

        for (size_t SeedIdx = 0; SeedIdx < SeedCpus.size(); ++SeedIdx) {
          pid_t DonorPid = fork();
          if (DonorPid == 0) {
            const int SeedCpu = SeedCpus[SeedIdx];

            // Linux: first donor uses attach-env=0 to create a fresh named shm.
            // Android: child must always attach to inherited backing fds; the
            // parent process already reset the pool just before forking.
#if SCUDO_ANDROID
            const char *AttachEnv = "1";
#else
            const char *AttachEnv =
                (SeedIdx == 0) ? "0" : "1";
#endif

            char DelegatedBuf[8];
            char ForceOffBuf[8];
            char SizeBuf[64];
            char WarmupBuf[32];
            char CpuBuf[32];
            char HoldUsBuf[32];
            char TouchPagesBuf[8];
            snprintf(DelegatedBuf, sizeof(DelegatedBuf), "%llu",
                     static_cast<unsigned long long>(Delegated ? 1 : 0));
            snprintf(ForceOffBuf, sizeof(ForceOffBuf), "%llu",
                     static_cast<unsigned long long>(!Delegated ? 1 : 0));
            snprintf(SizeBuf, sizeof(SizeBuf), "%llu",
                     static_cast<unsigned long long>(Size));
            snprintf(WarmupBuf, sizeof(WarmupBuf), "%u", Warmup);
            snprintf(CpuBuf, sizeof(CpuBuf), "%d", SeedCpu);
            snprintf(HoldUsBuf, sizeof(HoldUsBuf), "%llu",
                     static_cast<unsigned long long>(HoldUs));
            snprintf(TouchPagesBuf, sizeof(TouchPagesBuf), "%llu",
                     static_cast<unsigned long long>(TouchPages ? 1 : 0));

            execl(SelfExe, SelfExe, "--role", "donor", "--delegated",
                  DelegatedBuf,
                  "--attach-env", AttachEnv,
                  "--force-off-env", ForceOffBuf,
                  "--cpu", CpuBuf,
                  "--size", SizeBuf,
                  "--iterations", "0", "--warmup", WarmupBuf,
                  "--hold-us", HoldUsBuf,
                  "--touch-pages", TouchPagesBuf,
                  static_cast<char *>(nullptr));
            _exit(1);
          }
          (void)waitpid(DonorPid, nullptr, 0);
        }

        // 2) Workers: attach to the existing arena state.
        std::vector<uint64_t> CombinedSamples;
        CombinedSamples.clear();
        CombinedSamples.reserve(static_cast<size_t>(IndependentProcessCount) *
                                 (static_cast<size_t>(Iterations) * 2U + 1));

        std::vector<int> ReadFds;
        ReadFds.reserve(IndependentProcessCount);
        std::vector<pid_t> Pids;
        Pids.reserve(IndependentProcessCount);

        for (u32 P = 0; P < IndependentProcessCount; ++P) {
          int Fds[2];
          if (pipe(Fds) != 0) {
            fprintf(stderr, "pipe() failed errno=%d\n", errno);
            continue;
          }
          const int Cpu = (NumCores == 0)
                               ? 0
                               : ((BaseCpu + static_cast<int>(P)) %
                                  static_cast<int>(NumCores));
          const int SafeCpu = (Cpu < 0) ? (Cpu + static_cast<int>(NumCores))
                                        : Cpu;

          pid_t ChildPid = fork();
          if (ChildPid == 0) {
            close(Fds[0]);
            // Worker role writes raw samples to out-fd.
            char DelegatedBuf[8];
            char ForceOffBuf[8];
            char CpuBuf[32];
            char OutFdBuf[32];
            char SizeBuf[64];
            char IterBuf[32];
            char WarmupBuf[32];
            char HoldUsBuf[32];
            char TouchPagesBuf[8];
            snprintf(DelegatedBuf, sizeof(DelegatedBuf), "%llu",
                     static_cast<unsigned long long>(Delegated ? 1 : 0));
            snprintf(ForceOffBuf, sizeof(ForceOffBuf), "%llu",
                     static_cast<unsigned long long>(!Delegated ? 1 : 0));
            snprintf(CpuBuf, sizeof(CpuBuf), "%d", SafeCpu);
            snprintf(OutFdBuf, sizeof(OutFdBuf), "%d", Fds[1]);
            snprintf(SizeBuf, sizeof(SizeBuf), "%llu",
                     static_cast<unsigned long long>(Size));
            snprintf(IterBuf, sizeof(IterBuf), "%llu",
                     static_cast<unsigned long long>(Iterations));
            snprintf(WarmupBuf, sizeof(WarmupBuf), "%u", Warmup);
            snprintf(HoldUsBuf, sizeof(HoldUsBuf), "%llu",
                     static_cast<unsigned long long>(HoldUs));
            snprintf(TouchPagesBuf, sizeof(TouchPagesBuf), "%llu",
                     static_cast<unsigned long long>(TouchPages ? 1 : 0));
            execl(SelfExe, SelfExe, "--role", "worker", "--delegated",
                  DelegatedBuf,
                  "--attach-env", "1",
                  "--force-off-env", ForceOffBuf,
                  "--cpu", CpuBuf,
                  "--out-fd", OutFdBuf,
                  "--size", SizeBuf,
                  "--iterations", IterBuf,
                  "--warmup", WarmupBuf,
                  "--hold-us", HoldUsBuf,
                  "--touch-pages", TouchPagesBuf,
                  static_cast<char *>(nullptr));
            _exit(1);
          }

          close(Fds[1]);
          ReadFds.push_back(Fds[0]);
          Pids.push_back(ChildPid);
        }

        for (size_t I = 0; I < Pids.size(); ++I) {
          int Rfd = ReadFds[I];
          uint64_t Len = 0;
          if (!readExact(Rfd, &Len, sizeof(Len))) {
            close(Rfd);
            continue;
          }
          if (Len > 0) {
            size_t Old = CombinedSamples.size();
            CombinedSamples.resize(Old + Len);
            (void)readExact(Rfd, CombinedSamples.data() + Old,
                             Len * sizeof(uint64_t));
          }
          close(Rfd);
          int Status = 0;
          (void)waitpid(Pids[I], &Status, 0);
        }

        BenchSummary Summary;
        computeSummaryFromSamples(CombinedSamples, Summary);
        return Summary;
      };

      BenchSummary Traditional = runIndependent(false);
      BenchSummary Delegated = runIndependent(true);

      printf("\n================== size=%.2f %s, mode=independent-process (exec), unit=ns ==================\n",
             HumanSize, Unit);
      printf("-- delegated (SharedArena, donor seeds) --\n");
      printStatsWithDelta("  alloc_only", Delegated.Alloc, &Traditional.Alloc);
      printStatsWithDelta("  hold_only", Delegated.Hold, &Traditional.Hold);
      printStatsWithDelta("  dealloc_only", Delegated.Dealloc,
                           &Traditional.Dealloc);
      printStatsWithDelta("  all", Delegated.Round,
                           &Traditional.Round);
      printf("-- traditional (cache/mmap, Arena force-off) --\n");
      printStats("  alloc_only", Traditional.Alloc);
      printStats("  hold_only", Traditional.Hold);
      printStats("  dealloc_only", Traditional.Dealloc);
      printStats("  all", Traditional.Round);
      continue;
    }

    // Thread mode (existing behavior).
    auto runPath = [&](bool Delegated, const char *PathLabel) -> BenchSummary {
      BenchSummary Summary{};
      std::vector<uint64_t> Combined;
      if (ThreadCount <= 1) {
        runBenchRound(Alloc, Opt, Delegated, Size, Warmup, Iterations,
                      HoldUs, TouchPages, PathLabel, Combined);
      } else {
        u64 Per = Iterations / ThreadCount;
        if (Per == 0)
          Per = 1;
        std::vector<ThreadCtx> Ctxs(ThreadCount);
        std::vector<std::thread> Threads;
        for (u32 T = 0; T < ThreadCount; ++T) {
          Ctxs[T].Alloc = &Alloc;
          Ctxs[T].Opt = &Opt;
          Ctxs[T].Delegated = Delegated;
          Ctxs[T].Size = Size;
          Ctxs[T].Warmup = Warmup / ThreadCount + 1;
          Ctxs[T].Iterations = Per;
          Ctxs[T].HoldUs = HoldUs;
          Ctxs[T].TouchPages = TouchPages;
          Ctxs[T].Cpu =
              static_cast<int>(T) % static_cast<int>(Pool.getNumCores());
          Threads.emplace_back(threadMain, &Ctxs[T]);
        }
        for (auto &Th : Threads)
          Th.join();
        for (u32 T = 0; T < ThreadCount; ++T) {
          if (Ctxs[T].Err != 0)
            fprintf(stderr, "thread %u affinity errno=%d\n", T,
                    Ctxs[T].Err);
          Combined.insert(Combined.end(), Ctxs[T].Samples.begin(),
                           Ctxs[T].Samples.end());
        }
      }
      computeSummaryFromSamples(Combined, Summary);
      return Summary;
    };

    BenchSummary Traditional{};
    BenchSummary Delegated{};
    const bool HasDelegated = Pool.isReady();

    if (HasDelegated)
      Delegated = runPath(true, "delegated (SharedArena)");
    Traditional = runPath(false, "traditional (cache/mmap)");

    printf("\n================== size=%.2f %s, threads=%u, unit=ns ==================\n",
           HumanSize, Unit, ThreadCount);

    if (Pool.isReady()) {
      printf("-- delegated (SharedArena) --\n");
      printStatsWithDelta("  alloc_only", Delegated.Alloc, &Traditional.Alloc);
      printStatsWithDelta("  hold_only", Delegated.Hold, &Traditional.Hold);
      printStatsWithDelta("  dealloc_only", Delegated.Dealloc,
                           &Traditional.Dealloc);
      printStatsWithDelta("  all", Delegated.Round,
                           &Traditional.Round);
    } else {
      printf("-- delegated skipped (pool not ready) --\n");
    }
    printf("-- traditional (cache/mmap) --\n");
    printStats("  alloc_only", Traditional.Alloc);
    printStats("  hold_only", Traditional.Hold);
    printStats("  dealloc_only", Traditional.Dealloc);
    printStats("  all", Traditional.Round);
  }

  Alloc.unmapTestOnly();
  clearSharedArenaForceForTesting();
  return 0;
#endif
}
