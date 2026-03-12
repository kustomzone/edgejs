#ifndef UBI_HANDLE_WRAP_H_
#define UBI_HANDLE_WRAP_H_

#include <cstdint>

#include <uv.h>

#include "node_api.h"

enum UbiHandleState : uint8_t {
  kUbiHandleUninitialized = 0,
  kUbiHandleInitialized,
  kUbiHandleClosing,
  kUbiHandleClosed,
};

using UbiHandleWrapCloseCallback = void (*)(void* data);

struct UbiHandleWrap {
  napi_env env = nullptr;
  napi_ref wrapper_ref = nullptr;
  void* active_handle_token = nullptr;
  void* close_data = nullptr;
  uv_handle_t* uv_handle = nullptr;
  UbiHandleWrapCloseCallback close_callback = nullptr;
  UbiHandleWrap* prev = nullptr;
  UbiHandleWrap* next = nullptr;
  bool attached = false;
  bool finalized = false;
  bool delete_on_close = false;
  bool wrapper_ref_held = false;
  UbiHandleState state = kUbiHandleUninitialized;
};

void UbiHandleWrapInit(UbiHandleWrap* wrap, napi_env env);
void UbiHandleWrapAttach(UbiHandleWrap* wrap,
                         void* close_data,
                         uv_handle_t* handle,
                         UbiHandleWrapCloseCallback close_callback);
void UbiHandleWrapDetach(UbiHandleWrap* wrap);
napi_value UbiHandleWrapGetRefValue(napi_env env, napi_ref ref);
void UbiHandleWrapDeleteRefIfPresent(napi_env env, napi_ref* ref);
void UbiHandleWrapHoldWrapperRef(UbiHandleWrap* wrap);
void UbiHandleWrapReleaseWrapperRef(UbiHandleWrap* wrap);
bool UbiHandleWrapCancelFinalizer(UbiHandleWrap* wrap, void* native_object);
napi_value UbiHandleWrapGetActiveOwner(napi_env env, napi_ref wrapper_ref);
void UbiHandleWrapSetOnCloseCallback(napi_env env, napi_value wrapper, napi_value callback);
void UbiHandleWrapMaybeCallOnClose(UbiHandleWrap* wrap);
bool UbiHandleWrapHasRef(const UbiHandleWrap* wrap, const uv_handle_t* handle);
bool UbiHandleWrapEnvCleanupStarted(napi_env env);
void UbiHandleWrapRunEnvCleanup(napi_env env);

#endif  // UBI_HANDLE_WRAP_H_
