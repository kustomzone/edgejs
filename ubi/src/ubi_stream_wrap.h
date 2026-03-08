#ifndef UBI_STREAM_WRAP_H_
#define UBI_STREAM_WRAP_H_

#include <cstdint>

#include "node_api.h"

enum UbiStreamStateIndex : int {
  kUbiReadBytesOrError = 0,
  kUbiArrayBufferOffset = 1,
  kUbiBytesWritten = 2,
  kUbiLastWriteWasAsync = 3,
  kUbiStreamStateLength = 4,
};

napi_value UbiInstallStreamWrapBinding(napi_env env);
int32_t* UbiGetStreamBaseState(napi_env env);
napi_value UbiCreateStreamReqObject(napi_env env);

int64_t UbiStreamReqGetAsyncId(napi_env env, napi_value req_obj);
int32_t UbiStreamReqGetProviderType(napi_env env, napi_value req_obj);
void UbiStreamReqActivate(napi_env env,
                          napi_value req_obj,
                          int32_t provider_type,
                          int64_t trigger_async_id);
void UbiStreamReqMarkDone(napi_env env, napi_value req_obj);

#endif  // UBI_STREAM_WRAP_H_
