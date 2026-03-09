#!/usr/bin/env sh

export WASIXCC_WASM_EXCEPTIONS=yes

WASIXCC_DRIVER="${1:-wasixcc}"
WASIXCC_DEFAULT_SYSROOT="$("${WASIXCC_DRIVER}" --print-sysroot 2>/dev/null || true)"
WASIXCC_SYSROOT_PREFIX="$(dirname "${WASIXCC_DEFAULT_SYSROOT}")"
export WASIXCC_SYSROOT="${WASIXCC_SYSROOT_PREFIX}/sysroot-exnref-eh"
