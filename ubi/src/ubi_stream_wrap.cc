#include "ubi_stream_wrap.h"

#include <cstdint>

#include "ubi_async_wrap.h"

namespace {

int32_t* g_stream_state = nullptr;

struct StreamReqWrap {
  napi_env env = nullptr;
  napi_ref wrapper_ref = nullptr;
  napi_ref active_ref = nullptr;
  int64_t async_id = -1;
  int32_t provider_type = kUbiProviderNone;
  bool destroy_queued = false;
  bool active = false;
};

void DeleteRefIfPresent(napi_env env, napi_ref* ref) {
  if (env == nullptr || ref == nullptr || *ref == nullptr) return;
  napi_delete_reference(env, *ref);
  *ref = nullptr;
}

napi_value GetRefValue(napi_env env, napi_ref ref) {
  if (env == nullptr || ref == nullptr) return nullptr;
  napi_value value = nullptr;
  if (napi_get_reference_value(env, ref, &value) != napi_ok || value == nullptr) return nullptr;
  return value;
}

StreamReqWrap* UnwrapStreamReqWrap(napi_env env, napi_value value) {
  if (env == nullptr || value == nullptr) return nullptr;
  StreamReqWrap* wrap = nullptr;
  if (napi_unwrap(env, value, reinterpret_cast<void**>(&wrap)) != napi_ok) return nullptr;
  return wrap;
}

void QueueDestroyIfNeeded(StreamReqWrap* wrap) {
  if (wrap == nullptr || wrap->destroy_queued || wrap->async_id <= 0) return;
  UbiAsyncWrapQueueDestroyId(wrap->env, wrap->async_id);
  wrap->destroy_queued = true;
}

void StreamReqWrapFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* wrap = static_cast<StreamReqWrap*>(data);
  if (wrap == nullptr) return;
  QueueDestroyIfNeeded(wrap);
  DeleteRefIfPresent(env != nullptr ? env : wrap->env, &wrap->active_ref);
  if (wrap->wrapper_ref != nullptr) napi_delete_reference(env, wrap->wrapper_ref);
  delete wrap;
}

napi_value StreamReqCtor(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  auto* wrap = new StreamReqWrap();
  wrap->env = env;
  napi_wrap(env, self, wrap, StreamReqWrapFinalize, nullptr, &wrap->wrapper_ref);
  return self;
}

napi_value StreamReqGetAsyncId(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  StreamReqWrap* wrap = UnwrapStreamReqWrap(env, self);
  napi_value out = nullptr;
  napi_create_int64(env, wrap != nullptr ? wrap->async_id : -1, &out);
  return out;
}

napi_value StreamReqGetProviderTypeValue(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  StreamReqWrap* wrap = UnwrapStreamReqWrap(env, self);
  napi_value out = nullptr;
  napi_create_int32(env, wrap != nullptr ? wrap->provider_type : kUbiProviderNone, &out);
  return out;
}

void SetNamedU32(napi_env env, napi_value obj, const char* key, uint32_t value) {
  napi_value v = nullptr;
  if (napi_create_uint32(env, value, &v) == napi_ok && v != nullptr) {
    napi_set_named_property(env, obj, key, v);
  }
}

}  // namespace

int32_t* UbiGetStreamBaseState() {
  return g_stream_state;
}

int64_t UbiStreamReqGetAsyncId(napi_env env, napi_value req_obj) {
  StreamReqWrap* wrap = UnwrapStreamReqWrap(env, req_obj);
  return wrap != nullptr ? wrap->async_id : -1;
}

int32_t UbiStreamReqGetProviderType(napi_env env, napi_value req_obj) {
  StreamReqWrap* wrap = UnwrapStreamReqWrap(env, req_obj);
  return wrap != nullptr ? wrap->provider_type : kUbiProviderNone;
}

void UbiStreamReqActivate(napi_env env,
                          napi_value req_obj,
                          int32_t provider_type,
                          int64_t trigger_async_id) {
  StreamReqWrap* wrap = UnwrapStreamReqWrap(env, req_obj);
  if (wrap == nullptr) return;

  wrap->provider_type = provider_type;
  if (wrap->async_id <= 0 || wrap->destroy_queued) {
    wrap->async_id = UbiAsyncWrapNextId(env);
  } else {
    UbiAsyncWrapReset(env, &wrap->async_id);
  }
  wrap->destroy_queued = false;
  wrap->active = true;

  DeleteRefIfPresent(env, &wrap->active_ref);
  if (req_obj != nullptr) napi_create_reference(env, req_obj, 1, &wrap->active_ref);
  UbiAsyncWrapEmitInit(env, wrap->async_id, provider_type, trigger_async_id, req_obj);
}

void UbiStreamReqMarkDone(napi_env env, napi_value req_obj) {
  StreamReqWrap* wrap = UnwrapStreamReqWrap(env, req_obj);
  if (wrap == nullptr) return;
  QueueDestroyIfNeeded(wrap);
  DeleteRefIfPresent(env, &wrap->active_ref);
  wrap->active = false;
}

napi_value UbiInstallStreamWrapBinding(napi_env env) {
  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) return nullptr;

  napi_property_descriptor req_props[] = {
      {"getAsyncId", nullptr, StreamReqGetAsyncId, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getProviderType",
       nullptr,
       StreamReqGetProviderTypeValue,
       nullptr,
       nullptr,
       nullptr,
       napi_default_method,
       nullptr},
  };

  napi_value write_wrap_ctor = nullptr;
  if (napi_define_class(env,
                        "WriteWrap",
                        NAPI_AUTO_LENGTH,
                        StreamReqCtor,
                        nullptr,
                        sizeof(req_props) / sizeof(req_props[0]),
                        req_props,
                        &write_wrap_ctor) != napi_ok ||
      write_wrap_ctor == nullptr) {
    return nullptr;
  }

  napi_value shutdown_wrap_ctor = nullptr;
  if (napi_define_class(env,
                        "ShutdownWrap",
                        NAPI_AUTO_LENGTH,
                        StreamReqCtor,
                        nullptr,
                        sizeof(req_props) / sizeof(req_props[0]),
                        req_props,
                        &shutdown_wrap_ctor) != napi_ok ||
      shutdown_wrap_ctor == nullptr) {
    return nullptr;
  }

  void* state_data = nullptr;
  napi_value state_ab = nullptr;
  if (napi_create_arraybuffer(env, sizeof(int32_t) * kUbiStreamStateLength, &state_data, &state_ab) != napi_ok ||
      state_ab == nullptr || state_data == nullptr) {
    return nullptr;
  }
  g_stream_state = static_cast<int32_t*>(state_data);
  for (int i = 0; i < kUbiStreamStateLength; i++) g_stream_state[i] = 0;

  napi_value stream_state = nullptr;
  if (napi_create_typedarray(env,
                             napi_int32_array,
                             kUbiStreamStateLength,
                             state_ab,
                             0,
                             &stream_state) != napi_ok ||
      stream_state == nullptr) {
    return nullptr;
  }

  napi_set_named_property(env, binding, "WriteWrap", write_wrap_ctor);
  napi_set_named_property(env, binding, "ShutdownWrap", shutdown_wrap_ctor);
  napi_set_named_property(env, binding, "streamBaseState", stream_state);

  SetNamedU32(env, binding, "kReadBytesOrError", kUbiReadBytesOrError);
  SetNamedU32(env, binding, "kArrayBufferOffset", kUbiArrayBufferOffset);
  SetNamedU32(env, binding, "kBytesWritten", kUbiBytesWritten);
  SetNamedU32(env, binding, "kLastWriteWasAsync", kUbiLastWriteWasAsync);

  return binding;
}
