#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <test-name>" >&2
  echo "test-name maps to tests/programs/<test-name>.c" >&2
  exit 1
fi

TEST_NAME="$1"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")"/.. && pwd)"
PROJECT_ROOT="$ROOT_DIR/../.."
OUT_DIR="$ROOT_DIR/target/wasm32-wasix/release"
OUT_FILE="$OUT_DIR/${TEST_NAME}.wasm"
TEST_SRC="$ROOT_DIR/tests/programs/${TEST_NAME}.c"
NAPI_INCLUDE_DIR="$PROJECT_ROOT/napi/include"
TEST_INCLUDE_DIR="$ROOT_DIR/tests/programs"

if [[ ! -f "$TEST_SRC" ]]; then
  echo "test not found: $TEST_SRC" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

if [[ -f "$OUT_FILE" && "$OUT_FILE" -nt "$TEST_SRC" ]]; then
  echo "Up-to-date: $OUT_FILE"
  exit 0
fi

# Compile to WASIX. N-API functions become WASM imports (module "napi")
# thanks to __attribute__((__import_module__("napi"))) in the headers.
wasixcc \
  --target=wasm32-wasix \
  -O2 \
  -DBUILDING_NODE_EXTENSION \
  -DNAPI_VERSION=8 \
  -I"$NAPI_INCLUDE_DIR" \
  -I"$TEST_INCLUDE_DIR" \
  -Wl,--allow-undefined \
  -Wl,--export-memory \
  -Wl,--export=main \
  -Wl,--export=malloc \
  -Wl,--export=free \
  "$TEST_SRC" \
  -o "$OUT_FILE"

echo "Built: $OUT_FILE"
