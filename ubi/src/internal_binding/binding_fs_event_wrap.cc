#include "internal_binding/dispatch.h"

#include <unordered_map>

#include "internal_binding/helpers.h"

namespace internal_binding {

namespace {

struct FsEventWrap {
  uint32_t async_id = 0;
};

std::unordered_map<napi_env, uint32_t> g_next_async_id;

void FsEventFinalize(napi_env /*env*/, void* data, void* /*hint*/) {
  delete static_cast<FsEventWrap*>(data);
}

FsEventWrap* GetFsEventWrap(napi_env env, napi_callback_info info, napi_value* this_arg_out = nullptr) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  if (this_arg_out != nullptr) *this_arg_out = this_arg;
  if (this_arg == nullptr) return nullptr;
  void* data = nullptr;
  if (napi_unwrap(env, this_arg, &data) != napi_ok || data == nullptr) return nullptr;
  return static_cast<FsEventWrap*>(data);
}

napi_value FsEventCtor(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  if (this_arg == nullptr) return nullptr;

  auto* wrap = new FsEventWrap();
  wrap->async_id = ++g_next_async_id[env];
  if (napi_wrap(env, this_arg, wrap, FsEventFinalize, nullptr, nullptr) != napi_ok) {
    delete wrap;
    return nullptr;
  }
  return this_arg;
}

napi_value FsEventStart(napi_env env, napi_callback_info /*info*/) {
  napi_value out = nullptr;
  napi_create_int32(env, 0, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value FsEventClose(napi_env env, napi_callback_info /*info*/) {
  return Undefined(env);
}

napi_value FsEventRef(napi_env env, napi_callback_info /*info*/) {
  return Undefined(env);
}

napi_value FsEventUnref(napi_env env, napi_callback_info /*info*/) {
  return Undefined(env);
}

napi_value FsEventGetAsyncId(napi_env env, napi_callback_info info) {
  FsEventWrap* wrap = GetFsEventWrap(env, info);
  const uint32_t async_id = wrap != nullptr ? wrap->async_id : 0;
  napi_value out = nullptr;
  napi_create_uint32(env, async_id, &out);
  return out != nullptr ? out : Undefined(env);
}

}  // namespace

napi_value ResolveFsEventWrap(napi_env env, const ResolveOptions& /*options*/) {
  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) return Undefined(env);

  napi_property_descriptor methods[] = {
      {"start", nullptr, FsEventStart, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"close", nullptr, FsEventClose, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"ref", nullptr, FsEventRef, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"unref", nullptr, FsEventUnref, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getAsyncId", nullptr, FsEventGetAsyncId, nullptr, nullptr, nullptr, napi_default, nullptr},
  };

  napi_value cls = nullptr;
  if (napi_define_class(env,
                        "FSEvent",
                        NAPI_AUTO_LENGTH,
                        FsEventCtor,
                        nullptr,
                        sizeof(methods) / sizeof(methods[0]),
                        methods,
                        &cls) == napi_ok &&
      cls != nullptr) {
    napi_set_named_property(env, out, "FSEvent", cls);
  }

  return out;
}

}  // namespace internal_binding

