#!/usr/bin/env bash
# 本地 Linux 一键执行：
# 1) 配置 compiler-rt（含 tests）
# 2) 编译 ScudoSharedArenaLatencyBench
# 3) 本地直接运行
#
# 用法：
#   ./run_shared_arena_latency_bench.sh [bench参数...]
#
# 可选环境变量：
#   BUILD_DIR=/abs/path/to/build-dir
#   ARCH=x86_64 (默认)
#   CMAKE_EXTRA_ARGS="-D... -D..."
# 也支持传入 benchmark 的模式参数：
#   --mode thread|process|independent-process
#   --processes P        （仅 independent-process 模式使用）
#
# 示例：
#   ./run_shared_arena_latency_bench.sh --iterations 20000 --warmup 500 --threads 4
# 独立启动多进程（exec 的新进程）：
#   ./run_shared_arena_latency_bench.sh --mode independent-process --processes 4 --iterations 2000 --warmup 500 --threads 1

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPILERRT_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"

ARCH="${ARCH:-x86_64}"
BUILD_DIR="${BUILD_DIR:-${COMPILERRT_ROOT}/build-scudo-bench}"
TARGET="TScudoSharedArenaLatencyBench-${ARCH}-Test"
BENCH_LOCAL="${BUILD_DIR}/lib/scudo/standalone/tests/ScudoSharedArenaLatencyBench-${ARCH}-Test"

BENCH_ARGS=("$@")
if [[ ${#BENCH_ARGS[@]} -eq 0 ]]; then
  BENCH_ARGS=(--iterations 20000 --warmup 500 --threads 4)
fi

EXTRA_CMAKE_ARGS=()
if [[ -n "${CMAKE_EXTRA_ARGS:-}" ]]; then
  # shellcheck disable=SC2206
  EXTRA_CMAKE_ARGS=( ${CMAKE_EXTRA_ARGS} )
fi

if ! command -v cmake >/dev/null 2>&1; then
  echo "错误：未找到 cmake"
  exit 1
fi
if ! command -v ninja >/dev/null 2>&1; then
  echo "错误：未找到 ninja"
  exit 1
fi
if ! command -v clang >/dev/null 2>&1 || ! command -v clang++ >/dev/null 2>&1; then
  echo "错误：未找到 clang/clang++"
  exit 1
fi

CMAKE_ARGS=(
  -DCMAKE_BUILD_TYPE=Release
  -DCOMPILER_RT_INCLUDE_TESTS=ON
  -DCOMPILER_RT_SCUDO_STANDALONE_BUILD_SHARED=OFF
  -DCMAKE_C_COMPILER=clang
  -DCMAKE_CXX_COMPILER=clang++
)

if command -v llvm-config >/dev/null 2>&1; then
  CMAKE_ARGS+=( -DLLVM_CONFIG_PATH="$(command -v llvm-config)" )
fi

mkdir -p "${BUILD_DIR}"

echo "[1/3] 配置 CMake: ${BUILD_DIR}"
cmake -G Ninja -S "${COMPILERRT_ROOT}" -B "${BUILD_DIR}" \
  "${CMAKE_ARGS[@]}" \
  "${EXTRA_CMAKE_ARGS[@]}"

echo "[2/3] 编译目标: ${TARGET}"
cmake --build "${BUILD_DIR}" --target "${TARGET}" -j

if [[ ! -f "${BENCH_LOCAL}" ]]; then
  echo "错误：未找到可执行文件 ${BENCH_LOCAL}"
  echo "请检查 ARCH=${ARCH} 是否与构建配置一致。"
  exit 1
fi

echo "[3/3] 本地运行 benchmark"
echo "命令: ${BENCH_LOCAL} ${BENCH_ARGS[*]}"
"${BENCH_LOCAL}" "${BENCH_ARGS[@]}"
