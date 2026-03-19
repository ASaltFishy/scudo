#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<EOF
用法: $0 [输出路径] [--run-tests | --run-all-tests]

  无参数时:
    在当前 standalone 目录下生成 ./libscudo.so

  第一个非选项参数:
    作为输出 .so 路径，例如:
      $0 /tmp/libscudo.so

  --run-tests
    在构建完成后，编译并运行快速测试（仅 Secondary 相关）

  --run-all-tests
    在构建完成后，编译并运行 tests 目录下的全量 gtest
EOF
}

OUTPUT="$(pwd)/libscudo.so"
TEST_MODE="none"

for arg in "$@"; do
  case "$arg" in
    --help|-h)
      usage
      exit 0
      ;;
    --run-tests)
      if [ "$TEST_MODE" != "none" ]; then
        echo "--run-tests 和 --run-all-tests 不能同时指定"
        usage
        exit 1
      fi
      TEST_MODE="fast"
      ;;
    --run-all-tests)
      if [ "$TEST_MODE" != "none" ]; then
        echo "--run-tests 和 --run-all-tests 不能同时指定"
        usage
        exit 1
      fi
      TEST_MODE="all"
      ;;
    -*)
      echo "未知选项: $arg"
      usage
      exit 1
      ;;
    *)
      # 第一个非选项参数作为输出路径
      if [ "$OUTPUT" = "$(pwd)/libscudo.so" ]; then
        OUTPUT="$arg"
      else
        echo "多余的位置参数: $arg"
        usage
        exit 1
      fi
      ;;
  esac
done

echo "Building libscudo.so -> ${OUTPUT}"

clang++ -fPIC -std=c++17 -msse4.2 -g -pthread -shared -w \
  -I ./include \
  ./*.cpp \
  -o "${OUTPUT}"

echo "Build finished."

if [ "$TEST_MODE" != "none" ]; then
  echo "Building and running gtests under ./tests (mode: ${TEST_MODE}) ..."

  if [ ! -d ./tests ]; then
    echo "tests 目录不存在，跳过测试。"
    exit 0
  fi

  if [ "$TEST_MODE" = "fast" ]; then
    TEST_SOURCES=(
      "./tests/secondary_test.cpp"
      "./tests/allocator_config_test.cpp"
    )
    TEST_BIN="./tests/scudo_secondary_tests"
  else
    TEST_SOURCES=(./tests/*.cpp)
    TEST_BIN="./tests/scudo_tests"
  fi

  for test_src in "${TEST_SOURCES[@]}"; do
    if [ ! -f "${test_src}" ]; then
      echo "缺少测试文件: ${test_src}"
      exit 1
    fi
  done

  echo "Compiling tests -> ${TEST_BIN}"

  clang++ -std=c++17 -g -pthread -w \
    -I . \
    -I ./include \
    -I ./tests \
    "${TEST_SOURCES[@]}" \
    "${OUTPUT}" \
    -lgtest -lgtest_main \
    -o "${TEST_BIN}"

  TEST_LOG="./tests/test_results.log"
  : > "${TEST_LOG}"

  echo "==> Running ${TEST_BIN}" | tee -a "${TEST_LOG}"

  if [ "$TEST_MODE" = "fast" ]; then
    echo "==> Enabling shared arena trace and force mode for fast tests" \
      | tee -a "${TEST_LOG}"
    SCUDO_SHARED_ARENA_FORCE=1 \
    SCUDO_SHARED_ARENA_TRACE=1 \
      "${TEST_BIN}" 2>&1 | tee -a "${TEST_LOG}"
  else
    SCUDO_SHARED_ARENA_TRACE=1 \
      "${TEST_BIN}" 2>&1 | tee -a "${TEST_LOG}"
  fi

  echo "All tests finished. Full output saved to ${TEST_LOG}"
fi

