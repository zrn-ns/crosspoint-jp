#!/usr/bin/env bash
set -euo pipefail

# OTA アップデートチャネルのホストテストを実行する。
#
# OtaUpdater 本体は HttpDownloader (Arduino / mbedTLS) 依存でホストではリンク
# できないため、そこから切り出した2つを検証する。
#   - ReleaseJsonParser: GitHub /releases API の実データを整形した fixture
#   - src/network/ReleaseVersion.h: チャネル選択ポリシー（実機と同じ関数）

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/ota_channel"
BINARY="$BUILD_DIR/OtaChannelTest"

mkdir -p "$BUILD_DIR"

SOURCES=(
  "$ROOT_DIR/test/ota_channel/OtaChannelTest.cpp"
  "$ROOT_DIR/lib/JsonParser/ReleaseJsonParser.cpp"
  "$ROOT_DIR/lib/JsonParser/StreamingJsonParser.cpp"
)

CXXFLAGS=(
  -std=c++20
  -O2
  -Wall
  -Wextra
  -pedantic
  -I"$ROOT_DIR"
  -I"$ROOT_DIR/lib/JsonParser"
)

echo "Building OTA channel test..."
c++ "${CXXFLAGS[@]}" "${SOURCES[@]}" -o "$BINARY"

echo "Running OTA channel test..."
"$BINARY" "$ROOT_DIR/test/ota_channel"

# ASan/UBSan でもう一度。パーサは固定長の char 配列を SAX コールバックから
# 書き換えるので、状態機械の取り違えが配列外アクセスとして出る。実機では
# RISC-V のアラインメント例外や静かなメモリ破壊になり追跡が難しい。
if [ "${OTA_TEST_SKIP_SANITIZERS:-0}" != "1" ]; then
  echo "Rebuilding with ASan/UBSan..."
  c++ "${CXXFLAGS[@]}" -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    "${SOURCES[@]}" -o "$BINARY-san"
  echo "Running under ASan/UBSan..."
  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$BINARY-san" "$ROOT_DIR/test/ota_channel"
fi
