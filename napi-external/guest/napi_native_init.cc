// Native-build initialization for N-API guest tests.
// Uses unofficial_napi_create_env() from napi-v8 to create a proper env
// with all V8 scopes managed correctly.
//
// This file is only used for native (non-WASM) builds.

#include "unofficial_napi.h"

static napi_env g_env = nullptr;
static void* g_scope = nullptr;

extern "C" napi_env napi_wasm_init_env(void) {
  if (g_env != nullptr) return g_env;
  napi_status s = unofficial_napi_create_env(8, &g_env, &g_scope);
  return (s == napi_ok) ? g_env : nullptr;
}
