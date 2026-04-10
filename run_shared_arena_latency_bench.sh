#!/usr/bin/env bash
# ScudoSharedArenaLatencyBench 一键脚本：
# - MODE=linux   : 本地 Linux 构建并运行（默认）
# - MODE=android : Android NDK 交叉编译，并可通过 ssh + adb 部署到手机
#
# 用法：
#   ./run_shared_arena_latency_bench.sh [bench参数...]
#
# 常用环境变量：
#   MODE=linux|android                 默认 linux
#   BUILD_DIR=/abs/path/to/build-dir
#   CMAKE_EXTRA_ARGS="-D... -D..."
#
# Linux 模式：
#   ARCH=x86_64                        默认 uname -m
#
# Android 模式：
#   ANDROID_NDK=/abs/path/to/ndk       未设置时自动探测常见位置
#   ANDROID_ABI=arm64-v8a              默认 arm64-v8a
#   ANDROID_PLATFORM=android-34        默认 android-34
#   ANDROID_RUN=0|1                    默认 1；为 0 时只编译不运行
#   ANDROID_RUN_MODE=ssh-adb|adb       默认 ssh-adb
#   REMOTE_HOST=lrc@192.168.60.62      ssh-adb 模式必填/默认
#   REMOTE_STAGE_DIR=/tmp/scudo-bench  远端中转目录
#   DEVICE_DIR=/data/local/tmp/scudo   手机端目录
#   ADB_SERIAL=<serial>                可选，远端/本地 adb 都会透传
#
# benchmark 参数透传，例如：
#   ./run_shared_arena_latency_bench.sh --iterations 20000 --warmup 500 --threads 4
#   ./run_shared_arena_latency_bench.sh --mode independent-process --processes 4 --iterations 2000 --warmup 500 --threads 1

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPILERRT_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"

MODE="${MODE:-linux}"
BUILD_DIR="${BUILD_DIR:-}"
BENCH_ARGS=("$@")
if [[ ${#BENCH_ARGS[@]} -eq 0 ]]; then
  BENCH_ARGS=(--iterations 20000 --warmup 500 --threads 4)
fi

EXTRA_CMAKE_ARGS=()
if [[ -n "${CMAKE_EXTRA_ARGS:-}" ]]; then
  # shellcheck disable=SC2206
  EXTRA_CMAKE_ARGS=( ${CMAKE_EXTRA_ARGS} )
fi

die() {
  echo "错误：$*" >&2
  exit 1
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "未找到命令 $1"
}

detect_android_ndk() {
  local candidates=()
  if [[ -n "${ANDROID_NDK:-}" ]]; then
    candidates+=("${ANDROID_NDK}")
  fi
  if [[ -n "${ANDROID_NDK_HOME:-}" ]]; then
    candidates+=("${ANDROID_NDK_HOME}")
  fi
  candidates+=(
    "/home/lrc/amx/android-ndk-r27d"
    "/home/lrc/patent/kernel/kernel_platform/prebuilts/ndk-r26"
    "/home/lrc/amx/op15/kernel_platform/prebuilts/ndk-r26"
    "/home/lrc/amx/android-ndk-r26"
  )

  local ndk
  for ndk in "${candidates[@]}"; do
    if [[ -f "${ndk}/build/cmake/android.toolchain.cmake" ]]; then
      echo "${ndk}"
      return 0
    fi
  done
  return 1
}

find_bench_binary() {
  local build_dir="$1"
  local arch="$2"
  local candidate="${build_dir}/lib/scudo/standalone/tests/ScudoSharedArenaLatencyBench-${arch}-Test"
  local manual_candidate="${build_dir}/ScudoSharedArenaLatencyBench-${arch}-Test"
  if [[ -f "${candidate}" ]]; then
    echo "${candidate}"
    return 0
  fi
  if [[ -f "${manual_candidate}" ]]; then
    echo "${manual_candidate}"
    return 0
  fi

  local matches=()
  local pattern="${build_dir}/lib/scudo/standalone/tests/ScudoSharedArenaLatencyBench-*-Test"
  local manual_pattern="${build_dir}/ScudoSharedArenaLatencyBench-*-Test"
  # shellcheck disable=SC2206
  matches=( ${pattern} )
  if [[ ${#matches[@]} -eq 1 && -f "${matches[0]}" ]]; then
    echo "${matches[0]}"
    return 0
  fi
  # shellcheck disable=SC2206
  matches=( ${manual_pattern} )
  if [[ ${#matches[@]} -eq 1 && -f "${matches[0]}" ]]; then
    echo "${matches[0]}"
    return 0
  fi
  return 1
}

run_local_linux() {
  local arch="${ARCH:-$(uname -m)}"
  local build_dir="${BUILD_DIR:-${COMPILERRT_ROOT}/build-scudo-bench}"
  local target="scudo-shared-arena-latency-bench"
  local bench_local

  need_cmd cmake
  need_cmd ninja
  need_cmd clang
  need_cmd clang++

  local cmake_args=(
    -DCMAKE_BUILD_TYPE=Release
    -DCOMPILER_RT_INCLUDE_TESTS=ON
    -DCOMPILER_RT_SCUDO_STANDALONE_BUILD_SHARED=OFF
    -DCMAKE_C_COMPILER=clang
    -DCMAKE_CXX_COMPILER=clang++
  )

  if command -v llvm-config >/dev/null 2>&1; then
    cmake_args+=( -DLLVM_CONFIG_PATH="$(command -v llvm-config)" )
  fi

  mkdir -p "${build_dir}"

  echo "[1/3] 配置 CMake (${MODE}): ${build_dir}"
  cmake -G Ninja -S "${COMPILERRT_ROOT}" -B "${build_dir}" \
    "${cmake_args[@]}" \
    "${EXTRA_CMAKE_ARGS[@]}"

  echo "[2/3] 编译目标: ${target}"
  cmake --build "${build_dir}" --target "${target}" -j

  bench_local="$(find_bench_binary "${build_dir}" "${arch}")" || \
    die "未找到 Linux benchmark 可执行文件，请检查 ARCH=${arch}"

  echo "[3/3] 本地运行 benchmark"
  echo "命令: ${bench_local} ${BENCH_ARGS[*]}"
  "${bench_local}" "${BENCH_ARGS[@]}"
}

run_android_bench_via_local_adb() {
  local bench_local="$1"
  local remote_bin
  local device_dir="${DEVICE_DIR:-/data/local/tmp/scudo}"
  local adb_serial="${ADB_SERIAL:-}"
  local adb_args=()
  local bench_arg_str

  need_cmd adb

  if [[ -n "${adb_serial}" ]]; then
    adb_args=(-s "${adb_serial}")
  fi

  remote_bin="$(basename "${bench_local}")"
  bench_arg_str="$(printf '%q ' "${BENCH_ARGS[@]}")"

  adb "${adb_args[@]}" shell "mkdir -p '${device_dir}'"
  adb "${adb_args[@]}" push "${bench_local}" "${device_dir}/${remote_bin}"
  adb "${adb_args[@]}" shell "chmod 755 '${device_dir}/${remote_bin}'"

  echo "[3/3] 通过本地 adb 在手机上运行 benchmark"
  adb "${adb_args[@]}" shell \
    "cd '${device_dir}' && export MALLOC_USE_APP_DEFAULTS=1 && './${remote_bin}' ${bench_arg_str}"
}

run_android_bench_via_ssh_adb() {
  local bench_local="$1"
  local remote_host="${REMOTE_HOST:-lrc@192.168.60.62}"
  local remote_stage_dir="${REMOTE_STAGE_DIR:-/tmp/scudo-bench}"
  local device_dir="${DEVICE_DIR:-/data/local/tmp/scudo}"
  local adb_serial="${ADB_SERIAL:-}"
  local remote_bin

  need_cmd ssh
  need_cmd scp

  remote_bin="$(basename "${bench_local}")"

  echo "[3/4] 检查远端 ssh: ${remote_host}"
  ssh -o BatchMode=yes -o ConnectTimeout=5 "${remote_host}" "echo connected" >/dev/null

  echo "[4/4] 通过 ssh + adb 部署并运行 benchmark"
  ssh -o BatchMode=yes "${remote_host}" "mkdir -p '${remote_stage_dir}'"
  scp "${bench_local}" "${remote_host}:${remote_stage_dir}/${remote_bin}"

  ssh -o BatchMode=yes "${remote_host}" bash -s -- \
    "${remote_stage_dir}" "${remote_bin}" "${device_dir}" "${adb_serial:-__EMPTY__}" "${BENCH_ARGS[@]}" <<'EOF'
set -euo pipefail

REMOTE_STAGE_DIR="$1"
shift
REMOTE_BIN="$1"
shift
DEVICE_DIR="$1"
shift
ADB_SERIAL="$1"
shift
BENCH_ARGS=("$@")

if [[ "${ADB_SERIAL}" == "__EMPTY__" ]]; then
  ADB_SERIAL=""
fi

ADB_BIN="$(command -v adb || true)"
if [[ -z "${ADB_BIN}" && -x /opt/homebrew/bin/adb ]]; then
  ADB_BIN="/opt/homebrew/bin/adb"
fi
if [[ -z "${ADB_BIN}" ]]; then
  echo "错误：远端机器未找到 adb" >&2
  exit 1
fi

BENCH_ARG_STR="$(printf '%q ' "${BENCH_ARGS[@]}")"

adb_cmd() {
  if [[ -n "${ADB_SERIAL}" ]]; then
    "${ADB_BIN}" -s "${ADB_SERIAL}" "$@"
  else
    "${ADB_BIN}" "$@"
  fi
}

adb_cmd shell "mkdir -p '${DEVICE_DIR}'"
adb_cmd push "${REMOTE_STAGE_DIR}/${REMOTE_BIN}" "${DEVICE_DIR}/${REMOTE_BIN}"
adb_cmd shell "chmod 755 '${DEVICE_DIR}/${REMOTE_BIN}'"
adb_cmd shell \
  "cd '${DEVICE_DIR}' && export MALLOC_USE_APP_DEFAULTS=1 && './${REMOTE_BIN}' ${BENCH_ARG_STR}"
EOF
}

manual_android_build() {
  local ndk="$1"
  local api_level="$2"
  local arch="$3"
  local build_dir="$4"
  local clang_bin
  local output
  local compile_sources=(
    "${SCRIPT_DIR}/checksum.cpp"
    "${SCRIPT_DIR}/common.cpp"
    "${SCRIPT_DIR}/condition_variable_linux.cpp"
    "${SCRIPT_DIR}/crc32_hw.cpp"
    "${SCRIPT_DIR}/flags_parser.cpp"
    "${SCRIPT_DIR}/flags.cpp"
    "${SCRIPT_DIR}/fuchsia.cpp"
    "${SCRIPT_DIR}/linux.cpp"
    "${SCRIPT_DIR}/mem_map.cpp"
    "${SCRIPT_DIR}/mem_map_fuchsia.cpp"
    "${SCRIPT_DIR}/mem_map_linux.cpp"
    "${SCRIPT_DIR}/release.cpp"
    "${SCRIPT_DIR}/report.cpp"
    "${SCRIPT_DIR}/report_linux.cpp"
    "${SCRIPT_DIR}/shared_arena_linux.cpp"
    "${SCRIPT_DIR}/string_utils.cpp"
    "${SCRIPT_DIR}/timing.cpp"
    "${SCRIPT_DIR}/tests/shared_arena_latency_bench.cpp"
  )
  local compile_flags=(
    -std=c++17
    -O3
    -g
    -fPIE
    -pie
    -fno-exceptions
    -fno-emulated-tls
    -static-libstdc++
    -pthread
    -I "${SCRIPT_DIR}"
    -I "${COMPILERRT_ROOT}/lib"
    -I "${COMPILERRT_ROOT}/include"
    -I "${SCRIPT_DIR}/include"
  )
  local link_flags=(
    -ldl
    -latomic
    -llog
    -landroid
  )

  case "${arch}" in
    aarch64)
      clang_bin="${ndk}/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android${api_level}-clang++"
      ;;
    arm)
      clang_bin="${ndk}/toolchains/llvm/prebuilt/linux-x86_64/bin/armv7a-linux-androideabi${api_level}-clang++"
      ;;
    x86_64)
      clang_bin="${ndk}/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android${api_level}-clang++"
      ;;
    i686)
      clang_bin="${ndk}/toolchains/llvm/prebuilt/linux-x86_64/bin/i686-linux-android${api_level}-clang++"
      ;;
    *)
      die "手工 Android 编译不支持的架构 ${arch}"
      ;;
  esac

  [[ -x "${clang_bin}" ]] || die "未找到 NDK clang++: ${clang_bin}"

  output="${build_dir}/ScudoSharedArenaLatencyBench-${arch}-Test"
  echo "[2/4] Android CMake 未生成 benchmark target，回退到手工 NDK 编译"
  "${clang_bin}" \
    "${compile_flags[@]}" \
    "${compile_sources[@]}" \
    "${link_flags[@]}" \
    -o "${output}"
}

run_android() {
  local ndk
  local abi="${ANDROID_ABI:-arm64-v8a}"
  local android_platform="${ANDROID_PLATFORM:-android-34}"
  local android_run="${ANDROID_RUN:-1}"
  local android_run_mode="${ANDROID_RUN_MODE:-ssh-adb}"
  local arch
  local build_dir
  local target="scudo-shared-arena-latency-bench"
  local bench_local
  local api_level="${android_platform#android-}"

  need_cmd cmake
  need_cmd ninja
  need_cmd llvm-config

  ndk="$(detect_android_ndk)" || die "未找到 Android NDK，请设置 ANDROID_NDK"

  case "${abi}" in
    arm64-v8a) arch="aarch64" ;;
    armeabi-v7a) arch="arm" ;;
    x86_64) arch="x86_64" ;;
    x86) arch="i686" ;;
    *) die "不支持的 ANDROID_ABI=${abi}" ;;
  esac

  build_dir="${BUILD_DIR:-${COMPILERRT_ROOT}/build-scudo-bench-android-${arch}}"

  local cmake_args=(
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_TOOLCHAIN_FILE="${ndk}/build/cmake/android.toolchain.cmake"
    -DANDROID_ABI="${abi}"
    -DANDROID_PLATFORM="${android_platform}"
    -DCOMPILER_RT_INCLUDE_TESTS=ON
    -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON
    -DCOMPILER_RT_SCUDO_STANDALONE_BUILD_SHARED=OFF
    -DLLVM_CONFIG_PATH="$(command -v llvm-config)"
  )

  mkdir -p "${build_dir}"

  echo "[1/4] 配置 Android CMake: ${build_dir}"
  echo "NDK: ${ndk}"
  cmake -G Ninja -S "${COMPILERRT_ROOT}" -B "${build_dir}" \
    "${cmake_args[@]}" \
    "${EXTRA_CMAKE_ARGS[@]}"

  echo "[2/4] 编译目标: ${target}"
  if ! cmake --build "${build_dir}" --target "${target}" -j; then
    manual_android_build "${ndk}" "${api_level}" "${arch}" "${build_dir}"
  fi

  bench_local="$(find_bench_binary "${build_dir}" "${arch}")" || \
    die "未找到 Android benchmark 可执行文件，请检查 ANDROID_ABI=${abi}"
  echo "已生成: ${bench_local}"

  if [[ "${android_run}" == "0" ]]; then
    echo "ANDROID_RUN=0，跳过运行。"
    return 0
  fi

  case "${android_run_mode}" in
    ssh-adb)
      run_android_bench_via_ssh_adb "${bench_local}"
      ;;
    adb)
      run_android_bench_via_local_adb "${bench_local}"
      ;;
    *)
      die "不支持的 ANDROID_RUN_MODE=${android_run_mode}"
      ;;
  esac
}

case "${MODE}" in
  linux)
    run_local_linux
    ;;
  android)
    run_android
    ;;
  *)
    die "不支持的 MODE=${MODE}，可选 linux|android"
    ;;
esac
