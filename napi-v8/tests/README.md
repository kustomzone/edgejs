# napi-v8 Tests

Tier-1 tests use GoogleTest and port Node fixtures.

## Porting Rule

- Tests should be ported from Node as fully/verbatim as possible.
- Do not rewrite upstream test intent unless unavoidable.
- Adapt only execution glue (runner/shim/build wiring) as needed.
- If an upstream source path uses direct V8 APIs, replace those paths with
  N-API usage while preserving behavior.

## Current Ported Tests

- `2_function_arguments`
- `3_callbacks`

## Build And Run

The gtest binary requires a V8 library to link against.

```bash
cmake -S napi-v8 -B napi-v8/build -DNAPI_V8_V8_MONOLITH_LIB=/absolute/path/to/libv8_monolith.a
cmake --build napi-v8/build -j4
ctest --test-dir napi-v8/build --output-on-failure -R napi_v8_tier1_tests
```

If `NAPI_V8_V8_MONOLITH_LIB` is not set, the core `napi_v8` library still builds
and the gtest executable is skipped.

## Local Homebrew V8 Shortcut

Use the helper script for local testing with Homebrew V8 paths:

```bash
./napi-v8/scripts/test-local-v8.sh
```
