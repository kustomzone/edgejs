#ifndef UNOFFICIAL_NAPI_H_
#define UNOFFICIAL_NAPI_H_

#include "js_native_api.h"

#ifdef __cplusplus
extern "C" {
#endif

// Unofficial/test-only helper APIs for creating and tearing down an env scope.
NAPI_EXTERN napi_status unofficial_napi_open_env_scope(int32_t module_api_version,
                                                       napi_env* env_out,
                                                       void** scope_out);
NAPI_EXTERN napi_status unofficial_napi_close_env_scope(void* scope);

// Unofficial/test-only helper. Requests a full GC cycle for testing.
NAPI_EXTERN napi_status unofficial_napi_request_gc_for_testing(napi_env env);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // UNOFFICIAL_NAPI_H_
