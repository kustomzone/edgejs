#include "ubi_tls_wrap.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

#include <uv.h>

#include "ubi_runtime.h"
#include "ubi_stream_wrap.h"

namespace {

constexpr const char* kParentOwnerKey = "__ubi_tls_wrap_owner";

struct TlsWrap {
  napi_env env = nullptr;
  napi_ref wrapper_ref = nullptr;
  napi_ref parent_ref = nullptr;
  napi_ref session_ref = nullptr;
  bool is_server = false;
  bool handshake_started = false;
  bool handshake_done = false;
  int64_t async_id = 0;
  std::string servername;
};

struct TlsBindingState {
  napi_ref binding_ref = nullptr;
  napi_ref tls_wrap_ctor_ref = nullptr;
  int64_t next_async_id = 300000;
};

std::unordered_map<napi_env, TlsBindingState> g_tls_states;

napi_value Undefined(napi_env env) {
  napi_value out = nullptr;
  napi_get_undefined(env, &out);
  return out;
}

napi_value Null(napi_env env) {
  napi_value out = nullptr;
  napi_get_null(env, &out);
  return out;
}

napi_value MakeInt32(napi_env env, int32_t value) {
  napi_value out = nullptr;
  napi_create_int32(env, value, &out);
  return out;
}

napi_value MakeInt64(napi_env env, int64_t value) {
  napi_value out = nullptr;
  napi_create_int64(env, value, &out);
  return out;
}

napi_value MakeBool(napi_env env, bool value) {
  napi_value out = nullptr;
  napi_get_boolean(env, value, &out);
  return out;
}

napi_value GetRefValue(napi_env env, napi_ref ref) {
  if (ref == nullptr) return nullptr;
  napi_value out = nullptr;
  if (napi_get_reference_value(env, ref, &out) != napi_ok) return nullptr;
  return out;
}

void SetState(int idx, int32_t value) {
  int32_t* state = UbiGetStreamBaseState();
  if (state == nullptr) return;
  state[idx] = value;
}

TlsBindingState& EnsureState(napi_env env) {
  return g_tls_states[env];
}

TlsWrap* UnwrapTlsWrap(napi_env env, napi_value self) {
  if (env == nullptr || self == nullptr) return nullptr;
  TlsWrap* wrap = nullptr;
  if (napi_unwrap(env, self, reinterpret_cast<void**>(&wrap)) != napi_ok || wrap == nullptr) return nullptr;
  return wrap;
}

TlsWrap* UnwrapThis(napi_env env, napi_callback_info info, size_t* argc, napi_value* argv, napi_value* self_out) {
  size_t local_argc = (argc == nullptr) ? 0 : *argc;
  napi_value self = nullptr;
  if (napi_get_cb_info(env, info, &local_argc, argv, &self, nullptr) != napi_ok || self == nullptr) return nullptr;
  if (argc != nullptr) *argc = local_argc;
  if (self_out != nullptr) *self_out = self;
  return UnwrapTlsWrap(env, self);
}

napi_value GetParentHandle(TlsWrap* wrap) {
  if (wrap == nullptr) return nullptr;
  return GetRefValue(wrap->env, wrap->parent_ref);
}

int32_t CallParentMethodInt(TlsWrap* wrap,
                            const char* method,
                            size_t argc,
                            napi_value* argv,
                            napi_value* result_out = nullptr) {
  if (wrap == nullptr || method == nullptr) return UV_EINVAL;
  napi_env env = wrap->env;
  napi_value parent = GetParentHandle(wrap);
  if (parent == nullptr) return UV_EINVAL;
  napi_value fn = nullptr;
  napi_valuetype type = napi_undefined;
  if (napi_get_named_property(env, parent, method, &fn) != napi_ok ||
      fn == nullptr ||
      napi_typeof(env, fn, &type) != napi_ok ||
      type != napi_function) {
    return UV_EINVAL;
  }
  napi_value result = nullptr;
  if (napi_call_function(env, parent, fn, argc, argv, &result) != napi_ok || result == nullptr) {
    return UV_EINVAL;
  }
  if (result_out != nullptr) *result_out = result;
  int32_t rc = 0;
  if (napi_get_value_int32(env, result, &rc) == napi_ok) return rc;
  return 0;
}

napi_value CallParentMethodValue(TlsWrap* wrap, const char* method, size_t argc, napi_value* argv) {
  if (wrap == nullptr || method == nullptr) return nullptr;
  napi_env env = wrap->env;
  napi_value parent = GetParentHandle(wrap);
  if (parent == nullptr) return nullptr;
  napi_value fn = nullptr;
  napi_valuetype type = napi_undefined;
  if (napi_get_named_property(env, parent, method, &fn) != napi_ok ||
      fn == nullptr ||
      napi_typeof(env, fn, &type) != napi_ok ||
      type != napi_function) {
    return nullptr;
  }
  napi_value result = nullptr;
  if (napi_call_function(env, parent, fn, argc, argv, &result) != napi_ok || result == nullptr) return nullptr;
  return result;
}

napi_value GetNamedValue(napi_env env, napi_value obj, const char* key) {
  if (env == nullptr || obj == nullptr || key == nullptr) return nullptr;
  napi_value out = nullptr;
  if (napi_get_named_property(env, obj, key, &out) != napi_ok || out == nullptr) return nullptr;
  return out;
}

void EmitHandshakeCallbacks(TlsWrap* wrap, napi_value self) {
  if (wrap == nullptr || self == nullptr || wrap->handshake_done) return;

  napi_env env = wrap->env;
  napi_valuetype type = napi_undefined;

  if (!wrap->handshake_started) {
    wrap->handshake_started = true;
    napi_value on_start = GetNamedValue(env, self, "onhandshakestart");
    if (on_start != nullptr &&
        napi_typeof(env, on_start, &type) == napi_ok &&
        type == napi_function) {
      const int64_t now_ms = static_cast<int64_t>(uv_hrtime() / 1000000ULL);
      napi_value arg = MakeInt64(env, now_ms);
      napi_value argv[1] = {arg};
      napi_value ignored = nullptr;
      UbiMakeCallback(env, self, on_start, 1, argv, &ignored);
    }
  }

  napi_value on_done = GetNamedValue(env, self, "onhandshakedone");
  if (on_done != nullptr &&
      napi_typeof(env, on_done, &type) == napi_ok &&
      type == napi_function) {
    napi_value ignored = nullptr;
    UbiMakeCallback(env, self, on_done, 0, nullptr, &ignored);
  }
  wrap->handshake_done = true;
}

napi_value TlsWrapEmitHandshakeTick(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1 || argv[0] == nullptr) return Undefined(env);
  TlsWrap* wrap = UnwrapTlsWrap(env, argv[0]);
  if (wrap == nullptr || !wrap->is_server) return Undefined(env);
  EmitHandshakeCallbacks(wrap, argv[0]);
  return Undefined(env);
}

void QueueServerHandshakeTick(TlsWrap* wrap, napi_value self) {
  if (wrap == nullptr || self == nullptr || !wrap->is_server || wrap->handshake_done) return;

  napi_env env = wrap->env;
  napi_value global = nullptr;
  napi_value process = nullptr;
  napi_value next_tick = nullptr;
  napi_valuetype next_tick_type = napi_undefined;
  if (napi_get_global(env, &global) != napi_ok ||
      global == nullptr ||
      napi_get_named_property(env, global, "process", &process) != napi_ok ||
      process == nullptr ||
      napi_get_named_property(env, process, "nextTick", &next_tick) != napi_ok ||
      next_tick == nullptr ||
      napi_typeof(env, next_tick, &next_tick_type) != napi_ok ||
      next_tick_type != napi_function) {
    return;
  }

  napi_value callback = nullptr;
  if (napi_create_function(env,
                           "__ubiTlsServerHandshakeTick",
                           NAPI_AUTO_LENGTH,
                           TlsWrapEmitHandshakeTick,
                           nullptr,
                           &callback) != napi_ok ||
      callback == nullptr) {
    return;
  }

  napi_value argv[2] = {callback, self};
  napi_value ignored = nullptr;
  napi_call_function(env, process, next_tick, 2, argv, &ignored);
}

void ClearParentOwner(TlsWrap* wrap) {
  if (wrap == nullptr) return;
  napi_value parent = GetParentHandle(wrap);
  if (parent == nullptr) return;
  napi_value undefined = Undefined(wrap->env);
  napi_set_named_property(wrap->env, parent, kParentOwnerKey, undefined);
}

void TlsWrapFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* wrap = static_cast<TlsWrap*>(data);
  if (wrap == nullptr) return;
  ClearParentOwner(wrap);
  if (wrap->parent_ref != nullptr) {
    napi_delete_reference(env, wrap->parent_ref);
    wrap->parent_ref = nullptr;
  }
  if (wrap->session_ref != nullptr) {
    napi_delete_reference(env, wrap->session_ref);
    wrap->session_ref = nullptr;
  }
  if (wrap->wrapper_ref != nullptr) {
    napi_delete_reference(env, wrap->wrapper_ref);
    wrap->wrapper_ref = nullptr;
  }
  delete wrap;
}

napi_value ForwardParentRead(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value parent = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &parent, nullptr);
  if (parent == nullptr) return Undefined(env);

  napi_value owner = nullptr;
  if (napi_get_named_property(env, parent, kParentOwnerKey, &owner) != napi_ok || owner == nullptr) {
    return Undefined(env);
  }
  napi_valuetype owner_type = napi_undefined;
  if (napi_typeof(env, owner, &owner_type) != napi_ok || owner_type != napi_object) return Undefined(env);

  napi_value onread = nullptr;
  napi_valuetype onread_type = napi_undefined;
  if (napi_get_named_property(env, owner, "onread", &onread) != napi_ok ||
      onread == nullptr ||
      napi_typeof(env, onread, &onread_type) != napi_ok ||
      onread_type != napi_function) {
    return Undefined(env);
  }

  napi_value ignored = nullptr;
  UbiMakeCallback(env, owner, onread, argc, argv, &ignored);
  return Undefined(env);
}

napi_value TlsWrapCtor(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  if (self == nullptr) return Undefined(env);

  auto* wrap = new TlsWrap();
  wrap->env = env;
  TlsBindingState& state = EnsureState(env);
  wrap->async_id = state.next_async_id++;
  if (napi_wrap(env, self, wrap, TlsWrapFinalize, nullptr, &wrap->wrapper_ref) != napi_ok) {
    delete wrap;
    return Undefined(env);
  }

  napi_set_named_property(env, self, "isStreamBase", MakeBool(env, true));
  napi_set_named_property(env, self, "reading", MakeBool(env, false));
  return self;
}

napi_value TlsWrapReadStart(napi_env env, napi_callback_info info) {
  size_t argc = 0;
  napi_value self = nullptr;
  TlsWrap* wrap = UnwrapThis(env, info, &argc, nullptr, &self);
  if (wrap == nullptr) return MakeInt32(env, UV_EINVAL);
  // In the current native shim, server-side handshake progression is tied to
  // read startup. Emit callbacks once to unblock `secureConnection` flows.
  if (wrap->is_server && self != nullptr) {
    EmitHandshakeCallbacks(wrap, self);
  }
  const int32_t rc = CallParentMethodInt(wrap, "readStart", 0, nullptr);
  napi_set_named_property(env, self, "reading", MakeBool(env, rc == 0));
  return MakeInt32(env, rc);
}

napi_value TlsWrapReadStop(napi_env env, napi_callback_info info) {
  size_t argc = 0;
  napi_value self = nullptr;
  TlsWrap* wrap = UnwrapThis(env, info, &argc, nullptr, &self);
  if (wrap == nullptr) return MakeInt32(env, UV_EINVAL);
  const int32_t rc = CallParentMethodInt(wrap, "readStop", 0, nullptr);
  napi_set_named_property(env, self, "reading", MakeBool(env, false));
  return MakeInt32(env, rc);
}

napi_value TlsWrapWriteBuffer(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || argc < 2) return MakeInt32(env, UV_EINVAL);
  return MakeInt32(env, CallParentMethodInt(wrap, "writeBuffer", argc, argv));
}

napi_value TlsWrapWritev(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || argc < 2) return MakeInt32(env, UV_EINVAL);
  return MakeInt32(env, CallParentMethodInt(wrap, "writev", argc, argv));
}

napi_value TlsWrapWriteLatin1String(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || argc < 2) return MakeInt32(env, UV_EINVAL);
  return MakeInt32(env, CallParentMethodInt(wrap, "writeLatin1String", argc, argv));
}

napi_value TlsWrapWriteUtf8String(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || argc < 2) return MakeInt32(env, UV_EINVAL);
  return MakeInt32(env, CallParentMethodInt(wrap, "writeUtf8String", argc, argv));
}

napi_value TlsWrapWriteAsciiString(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || argc < 2) return MakeInt32(env, UV_EINVAL);
  return MakeInt32(env, CallParentMethodInt(wrap, "writeAsciiString", argc, argv));
}

napi_value TlsWrapWriteUcs2String(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || argc < 2) return MakeInt32(env, UV_EINVAL);
  return MakeInt32(env, CallParentMethodInt(wrap, "writeUcs2String", argc, argv));
}

napi_value TlsWrapShutdown(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || argc < 1) return MakeInt32(env, UV_EINVAL);
  return MakeInt32(env, CallParentMethodInt(wrap, "shutdown", argc, argv));
}

napi_value TlsWrapClose(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr) return Undefined(env);
  ClearParentOwner(wrap);
  CallParentMethodInt(wrap, "close", argc, argv);
  return Undefined(env);
}

napi_value TlsWrapRef(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap != nullptr) {
    CallParentMethodInt(wrap, "ref", 0, nullptr);
  }
  return Undefined(env);
}

napi_value TlsWrapUnref(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap != nullptr) {
    CallParentMethodInt(wrap, "unref", 0, nullptr);
  }
  return Undefined(env);
}

napi_value TlsWrapGetAsyncId(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  return MakeInt64(env, wrap != nullptr ? wrap->async_id : -1);
}

napi_value TlsWrapGetProviderType(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr) return MakeInt32(env, 0);
  napi_value parent = GetParentHandle(wrap);
  if (parent == nullptr) return MakeInt32(env, 0);
  napi_value fn = GetNamedValue(env, parent, "getProviderType");
  napi_valuetype type = napi_undefined;
  if (fn != nullptr && napi_typeof(env, fn, &type) == napi_ok && type == napi_function) {
    napi_value result = nullptr;
    if (napi_call_function(env, parent, fn, 0, nullptr, &result) == napi_ok && result != nullptr) {
      return result;
    }
  }
  return MakeInt32(env, 0);
}

napi_value TlsWrapAsyncReset(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap != nullptr) {
    TlsBindingState& state = EnsureState(env);
    wrap->async_id = state.next_async_id++;
  }
  return Undefined(env);
}

napi_value TlsWrapUseUserBuffer(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap != nullptr) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_value self = nullptr;
    if (napi_get_cb_info(env, info, &argc, argv, &self, nullptr) == napi_ok && argc >= 1 && argv[0] != nullptr) {
      CallParentMethodInt(wrap, "useUserBuffer", argc, argv);
    }
  }
  return Undefined(env);
}

napi_value TlsWrapBytesReadGetter(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  TlsWrap* wrap = UnwrapTlsWrap(env, self);
  if (wrap == nullptr) return MakeInt32(env, 0);
  napi_value parent = GetParentHandle(wrap);
  if (parent == nullptr) return MakeInt32(env, 0);
  napi_value out = GetNamedValue(env, parent, "bytesRead");
  return out != nullptr ? out : MakeInt32(env, 0);
}

napi_value TlsWrapBytesWrittenGetter(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  TlsWrap* wrap = UnwrapTlsWrap(env, self);
  if (wrap == nullptr) return MakeInt32(env, 0);
  napi_value parent = GetParentHandle(wrap);
  if (parent == nullptr) return MakeInt32(env, 0);
  napi_value out = GetNamedValue(env, parent, "bytesWritten");
  return out != nullptr ? out : MakeInt32(env, 0);
}

napi_value TlsWrapFdGetter(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  TlsWrap* wrap = UnwrapTlsWrap(env, self);
  if (wrap == nullptr) return MakeInt32(env, -1);
  napi_value parent = GetParentHandle(wrap);
  if (parent == nullptr) return MakeInt32(env, -1);
  napi_value out = GetNamedValue(env, parent, "fd");
  return out != nullptr ? out : MakeInt32(env, -1);
}

napi_value TlsWrapSetVerifyMode(napi_env env, napi_callback_info info) {
  (void)info;
  return Undefined(env);
}

napi_value TlsWrapEnableTrace(napi_env env, napi_callback_info info) {
  (void)info;
  return Undefined(env);
}

napi_value TlsWrapEnableSessionCallbacks(napi_env env, napi_callback_info info) {
  (void)info;
  return Undefined(env);
}

napi_value TlsWrapEnableCertCb(napi_env env, napi_callback_info info) {
  (void)info;
  return Undefined(env);
}

napi_value TlsWrapEnableALPNCb(napi_env env, napi_callback_info info) {
  (void)info;
  return Undefined(env);
}

napi_value TlsWrapEnablePskCallback(napi_env env, napi_callback_info info) {
  (void)info;
  return Undefined(env);
}

napi_value TlsWrapSetPskIdentityHint(napi_env env, napi_callback_info info) {
  (void)info;
  return Undefined(env);
}

napi_value TlsWrapEnableKeylogCallback(napi_env env, napi_callback_info info) {
  (void)info;
  return Undefined(env);
}

napi_value TlsWrapWritesIssuedByPrevListenerDone(napi_env env, napi_callback_info info) {
  (void)info;
  return Undefined(env);
}

napi_value TlsWrapSetALPNProtocols(napi_env env, napi_callback_info info) {
  (void)info;
  return Undefined(env);
}

napi_value TlsWrapRequestOCSP(napi_env env, napi_callback_info info) {
  (void)info;
  return Undefined(env);
}

napi_value TlsWrapStart(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, &self);
  if (wrap == nullptr || self == nullptr) return Undefined(env);
  EmitHandshakeCallbacks(wrap, self);
  return Undefined(env);
}

napi_value TlsWrapRenegotiate(napi_env env, napi_callback_info info) {
  return TlsWrapStart(env, info);
}

napi_value TlsWrapSetServername(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr) return Undefined(env);
  if (argc >= 1 && argv[0] != nullptr) {
    size_t len = 0;
    if (napi_get_value_string_utf8(env, argv[0], nullptr, 0, &len) == napi_ok) {
      std::string s(len + 1, '\0');
      size_t copied = 0;
      if (napi_get_value_string_utf8(env, argv[0], s.data(), s.size(), &copied) == napi_ok) {
        s.resize(copied);
        wrap->servername = std::move(s);
      }
    }
  }
  return Undefined(env);
}

napi_value TlsWrapGetServername(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr || wrap->servername.empty()) return Undefined(env);
  napi_value out = nullptr;
  napi_create_string_utf8(env, wrap->servername.c_str(), wrap->servername.size(), &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value TlsWrapSetSession(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr) return Undefined(env);
  if (wrap->session_ref != nullptr) {
    napi_delete_reference(env, wrap->session_ref);
    wrap->session_ref = nullptr;
  }
  if (argc >= 1 && argv[0] != nullptr) {
    napi_create_reference(env, argv[0], 1, &wrap->session_ref);
  }
  return Undefined(env);
}

napi_value TlsWrapGetSession(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr || wrap->session_ref == nullptr) return Undefined(env);
  napi_value session = GetRefValue(env, wrap->session_ref);
  return session != nullptr ? session : Undefined(env);
}

napi_value TlsWrapExportKeyingMaterial(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  uint32_t length = 0;
  if (argc >= 1 && argv[0] != nullptr) napi_get_value_uint32(env, argv[0], &length);
  void* data = nullptr;
  napi_value buffer = nullptr;
  if (napi_create_buffer(env, length, &data, &buffer) != napi_ok || buffer == nullptr) return Undefined(env);
  if (data != nullptr && length > 0) std::memset(data, 0, length);
  return buffer;
}

napi_value TlsWrapSetMaxSendFragment(napi_env env, napi_callback_info info) {
  (void)info;
  return MakeInt32(env, 1);
}

napi_value TlsWrapGetALPNNegotiatedProtocol(napi_env env, napi_callback_info info) {
  (void)info;
  return MakeBool(env, false);
}

napi_value TlsWrapGetCipher(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  napi_value proxied = CallParentMethodValue(wrap, "getCipher", 0, nullptr);
  if (proxied != nullptr) return proxied;

  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) return Undefined(env);
  napi_value name = nullptr;
  napi_value version = nullptr;
  napi_value standard_name = nullptr;
  napi_create_string_utf8(env, "TLS_AES_128_GCM_SHA256", NAPI_AUTO_LENGTH, &name);
  napi_create_string_utf8(env, "TLSv1.3", NAPI_AUTO_LENGTH, &version);
  napi_create_string_utf8(env, "TLS_AES_128_GCM_SHA256", NAPI_AUTO_LENGTH, &standard_name);
  if (name != nullptr) napi_set_named_property(env, out, "name", name);
  if (version != nullptr) napi_set_named_property(env, out, "version", version);
  if (standard_name != nullptr) napi_set_named_property(env, out, "standardName", standard_name);
  return out;
}

napi_value TlsWrapGetSharedSigalgs(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  napi_value proxied = CallParentMethodValue(wrap, "getSharedSigalgs", 0, nullptr);
  if (proxied != nullptr) return proxied;
  napi_value out = nullptr;
  napi_create_array_with_length(env, 0, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value TlsWrapGetEphemeralKeyInfo(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  napi_value proxied = CallParentMethodValue(wrap, "getEphemeralKeyInfo", 0, nullptr);
  if (proxied != nullptr) return proxied;
  napi_value out = nullptr;
  napi_create_object(env, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value TlsWrapGetFinished(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  napi_value proxied = CallParentMethodValue(wrap, "getFinished", 0, nullptr);
  return proxied != nullptr ? proxied : Undefined(env);
}

napi_value TlsWrapGetPeerFinished(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  napi_value proxied = CallParentMethodValue(wrap, "getPeerFinished", 0, nullptr);
  return proxied != nullptr ? proxied : Undefined(env);
}

napi_value TlsWrapGetProtocol(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  napi_value proxied = CallParentMethodValue(wrap, "getProtocol", 0, nullptr);
  if (proxied != nullptr) return proxied;
  napi_value out = nullptr;
  napi_create_string_utf8(env, "TLSv1.3", NAPI_AUTO_LENGTH, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value TlsWrapGetTLSTicket(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  napi_value proxied = CallParentMethodValue(wrap, "getTLSTicket", 0, nullptr);
  return proxied != nullptr ? proxied : Undefined(env);
}

napi_value TlsWrapIsSessionReused(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  napi_value proxied = CallParentMethodValue(wrap, "isSessionReused", 0, nullptr);
  if (proxied != nullptr) return proxied;
  return MakeBool(env, false);
}

napi_value TlsWrapGetPeerX509Certificate(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  napi_value proxied = CallParentMethodValue(wrap, "getPeerX509Certificate", 0, nullptr);
  return proxied != nullptr ? proxied : Undefined(env);
}

napi_value TlsWrapGetX509Certificate(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  napi_value proxied = CallParentMethodValue(wrap, "getX509Certificate", 0, nullptr);
  return proxied != nullptr ? proxied : Undefined(env);
}

napi_value TlsWrapVerifyError(napi_env env, napi_callback_info info) {
  (void)info;
  return Null(env);
}

napi_value TlsWrapGetPeerCertificate(napi_env env, napi_callback_info info) {
  (void)info;
  return Undefined(env);
}

napi_value TlsWrapGetCertificate(napi_env env, napi_callback_info info) {
  (void)info;
  return Undefined(env);
}

napi_value TlsWrapSetKeyCert(napi_env env, napi_callback_info info) {
  (void)info;
  return Undefined(env);
}

napi_value TlsWrapDestroySSL(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap != nullptr) {
    ClearParentOwner(wrap);
  }
  return Undefined(env);
}

napi_value TlsWrapLoadSession(napi_env env, napi_callback_info info) {
  (void)info;
  return Undefined(env);
}

napi_value TlsWrapEndParser(napi_env env, napi_callback_info info) {
  (void)info;
  return Undefined(env);
}

napi_value TlsWrapSetOCSPResponse(napi_env env, napi_callback_info info) {
  (void)info;
  return Undefined(env);
}

napi_value TlsWrapCertCbDone(napi_env env, napi_callback_info info) {
  (void)info;
  return Undefined(env);
}

napi_value TlsWrapNewSessionDone(napi_env env, napi_callback_info info) {
  (void)info;
  return Undefined(env);
}

napi_value TlsWrapReceive(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, &self);
  if (wrap == nullptr || self == nullptr || argc < 1 || argv[0] == nullptr) return Undefined(env);

  napi_value data = argv[0];
  napi_value array_buffer = nullptr;
  int32_t offset = 0;
  int32_t length = 0;

  bool is_array_buffer = false;
  if (napi_is_arraybuffer(env, data, &is_array_buffer) == napi_ok && is_array_buffer) {
    void* raw = nullptr;
    size_t len = 0;
    if (napi_get_arraybuffer_info(env, data, &raw, &len) == napi_ok) {
      array_buffer = data;
      length = static_cast<int32_t>(len);
      offset = 0;
    }
  } else {
    napi_value ab = GetNamedValue(env, data, "buffer");
    bool is_ab = false;
    if (ab != nullptr && napi_is_arraybuffer(env, ab, &is_ab) == napi_ok && is_ab) {
      array_buffer = ab;
      napi_value off = GetNamedValue(env, data, "byteOffset");
      napi_value len_v = GetNamedValue(env, data, "byteLength");
      uint32_t off_u32 = 0;
      uint32_t len_u32 = 0;
      if (off != nullptr) napi_get_value_uint32(env, off, &off_u32);
      if (len_v != nullptr) napi_get_value_uint32(env, len_v, &len_u32);
      offset = static_cast<int32_t>(off_u32);
      length = static_cast<int32_t>(len_u32);
    }
  }

  if (array_buffer == nullptr) return Undefined(env);

  SetState(kUbiReadBytesOrError, length);
  SetState(kUbiArrayBufferOffset, offset);

  napi_value onread = GetNamedValue(env, self, "onread");
  napi_valuetype onread_type = napi_undefined;
  if (onread != nullptr &&
      napi_typeof(env, onread, &onread_type) == napi_ok &&
      onread_type == napi_function) {
    napi_value call_argv[1] = {array_buffer};
    napi_value ignored = nullptr;
    UbiMakeCallback(env, self, onread, 1, call_argv, &ignored);
  }
  return Undefined(env);
}

napi_value TlsWrapWrap(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1 || argv[0] == nullptr) return Undefined(env);

  TlsBindingState& state = EnsureState(env);
  napi_value ctor = GetRefValue(env, state.tls_wrap_ctor_ref);
  if (ctor == nullptr) return Undefined(env);

  napi_value out = nullptr;
  if (napi_new_instance(env, ctor, 0, nullptr, &out) != napi_ok || out == nullptr) {
    return Undefined(env);
  }

  TlsWrap* wrap = UnwrapTlsWrap(env, out);
  if (wrap == nullptr) return Undefined(env);
  if (wrap->parent_ref != nullptr) {
    napi_delete_reference(env, wrap->parent_ref);
    wrap->parent_ref = nullptr;
  }
  napi_create_reference(env, argv[0], 1, &wrap->parent_ref);

  if (argc >= 3 && argv[2] != nullptr) {
    bool is_server = false;
    if (napi_get_value_bool(env, argv[2], &is_server) == napi_ok) {
      wrap->is_server = is_server;
    }
  }

  napi_value parent_reading = GetNamedValue(env, argv[0], "reading");
  if (parent_reading != nullptr) {
    napi_set_named_property(env, out, "reading", parent_reading);
  } else {
    napi_set_named_property(env, out, "reading", MakeBool(env, false));
  }

  napi_set_named_property(env, argv[0], kParentOwnerKey, out);
  napi_value onread_forward = nullptr;
  if (napi_create_function(env,
                           "__ubiTlsParentOnRead",
                           NAPI_AUTO_LENGTH,
                           ForwardParentRead,
                           nullptr,
                           &onread_forward) == napi_ok &&
      onread_forward != nullptr) {
    napi_set_named_property(env, argv[0], "onread", onread_forward);
  }

  QueueServerHandshakeTick(wrap, out);

  return out;
}

napi_value UbiInstallTlsWrapBindingInternal(napi_env env) {
  TlsBindingState& state = EnsureState(env);
  napi_value cached = GetRefValue(env, state.binding_ref);
  if (cached != nullptr) return cached;

  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) return nullptr;

  constexpr napi_property_attributes kMutableMethod =
      static_cast<napi_property_attributes>(napi_writable | napi_configurable);
  napi_property_descriptor tls_wrap_props[] = {
      {"readStart", nullptr, TlsWrapReadStart, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"readStop", nullptr, TlsWrapReadStop, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"writeBuffer", nullptr, TlsWrapWriteBuffer, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"writev", nullptr, TlsWrapWritev, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"writeLatin1String", nullptr, TlsWrapWriteLatin1String, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"writeUtf8String", nullptr, TlsWrapWriteUtf8String, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"writeAsciiString", nullptr, TlsWrapWriteAsciiString, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"writeUcs2String", nullptr, TlsWrapWriteUcs2String, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"shutdown", nullptr, TlsWrapShutdown, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"close", nullptr, TlsWrapClose, nullptr, nullptr, nullptr, kMutableMethod, nullptr},
      {"ref", nullptr, TlsWrapRef, nullptr, nullptr, nullptr, kMutableMethod, nullptr},
      {"unref", nullptr, TlsWrapUnref, nullptr, nullptr, nullptr, kMutableMethod, nullptr},
      {"getAsyncId", nullptr, TlsWrapGetAsyncId, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getProviderType", nullptr, TlsWrapGetProviderType, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"asyncReset", nullptr, TlsWrapAsyncReset, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"useUserBuffer", nullptr, TlsWrapUseUserBuffer, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"setVerifyMode", nullptr, TlsWrapSetVerifyMode, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"enableTrace", nullptr, TlsWrapEnableTrace, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"enableSessionCallbacks", nullptr, TlsWrapEnableSessionCallbacks, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"enableCertCb", nullptr, TlsWrapEnableCertCb, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"enableALPNCb", nullptr, TlsWrapEnableALPNCb, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"enablePskCallback", nullptr, TlsWrapEnablePskCallback, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"setPskIdentityHint", nullptr, TlsWrapSetPskIdentityHint, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"enableKeylogCallback", nullptr, TlsWrapEnableKeylogCallback, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"writesIssuedByPrevListenerDone", nullptr, TlsWrapWritesIssuedByPrevListenerDone, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"setALPNProtocols", nullptr, TlsWrapSetALPNProtocols, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"requestOCSP", nullptr, TlsWrapRequestOCSP, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"start", nullptr, TlsWrapStart, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"renegotiate", nullptr, TlsWrapRenegotiate, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"setServername", nullptr, TlsWrapSetServername, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getServername", nullptr, TlsWrapGetServername, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"setSession", nullptr, TlsWrapSetSession, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getSession", nullptr, TlsWrapGetSession, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getCipher", nullptr, TlsWrapGetCipher, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getSharedSigalgs", nullptr, TlsWrapGetSharedSigalgs, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getEphemeralKeyInfo", nullptr, TlsWrapGetEphemeralKeyInfo, nullptr, nullptr, nullptr, napi_default_method,
       nullptr},
      {"getFinished", nullptr, TlsWrapGetFinished, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getPeerFinished", nullptr, TlsWrapGetPeerFinished, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getProtocol", nullptr, TlsWrapGetProtocol, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getTLSTicket", nullptr, TlsWrapGetTLSTicket, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"isSessionReused", nullptr, TlsWrapIsSessionReused, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getPeerX509Certificate", nullptr, TlsWrapGetPeerX509Certificate, nullptr, nullptr, nullptr, napi_default_method,
       nullptr},
      {"getX509Certificate", nullptr, TlsWrapGetX509Certificate, nullptr, nullptr, nullptr, napi_default_method,
       nullptr},
      {"exportKeyingMaterial", nullptr, TlsWrapExportKeyingMaterial, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"setMaxSendFragment", nullptr, TlsWrapSetMaxSendFragment, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getALPNNegotiatedProtocol", nullptr, TlsWrapGetALPNNegotiatedProtocol, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"verifyError", nullptr, TlsWrapVerifyError, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getPeerCertificate", nullptr, TlsWrapGetPeerCertificate, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getCertificate", nullptr, TlsWrapGetCertificate, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"setKeyCert", nullptr, TlsWrapSetKeyCert, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"destroySSL", nullptr, TlsWrapDestroySSL, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"loadSession", nullptr, TlsWrapLoadSession, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"endParser", nullptr, TlsWrapEndParser, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"setOCSPResponse", nullptr, TlsWrapSetOCSPResponse, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"certCbDone", nullptr, TlsWrapCertCbDone, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"newSessionDone", nullptr, TlsWrapNewSessionDone, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"receive", nullptr, TlsWrapReceive, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"bytesRead", nullptr, nullptr, TlsWrapBytesReadGetter, nullptr, nullptr, napi_default, nullptr},
      {"bytesWritten", nullptr, nullptr, TlsWrapBytesWrittenGetter, nullptr, nullptr, napi_default, nullptr},
      {"fd", nullptr, nullptr, TlsWrapFdGetter, nullptr, nullptr, napi_default, nullptr},
  };

  napi_value tls_wrap_ctor = nullptr;
  if (napi_define_class(env,
                        "TLSWrap",
                        NAPI_AUTO_LENGTH,
                        TlsWrapCtor,
                        nullptr,
                        sizeof(tls_wrap_props) / sizeof(tls_wrap_props[0]),
                        tls_wrap_props,
                        &tls_wrap_ctor) != napi_ok ||
      tls_wrap_ctor == nullptr) {
    return nullptr;
  }

  if (state.tls_wrap_ctor_ref != nullptr) {
    napi_delete_reference(env, state.tls_wrap_ctor_ref);
    state.tls_wrap_ctor_ref = nullptr;
  }
  napi_create_reference(env, tls_wrap_ctor, 1, &state.tls_wrap_ctor_ref);

  napi_value wrap_fn = nullptr;
  if (napi_create_function(env, "wrap", NAPI_AUTO_LENGTH, TlsWrapWrap, nullptr, &wrap_fn) != napi_ok ||
      wrap_fn == nullptr) {
    return nullptr;
  }

  napi_set_named_property(env, binding, "TLSWrap", tls_wrap_ctor);
  napi_set_named_property(env, binding, "wrap", wrap_fn);

  if (state.binding_ref != nullptr) {
    napi_delete_reference(env, state.binding_ref);
    state.binding_ref = nullptr;
  }
  napi_create_reference(env, binding, 1, &state.binding_ref);

  return binding;
}

}  // namespace

napi_value UbiInstallTlsWrapBinding(napi_env env) {
  if (env == nullptr) return nullptr;
  return UbiInstallTlsWrapBindingInternal(env);
}
