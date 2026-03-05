#include "ubi_js_stream.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <uv.h>

#include "ubi_runtime.h"
#include "ubi_stream_wrap.h"

namespace {

struct JsStreamWrap {
  napi_env env = nullptr;
  napi_ref wrapper_ref = nullptr;
  uint64_t bytes_read = 0;
  uint64_t bytes_written = 0;
  int64_t async_id = 0;
};

int64_t g_next_js_stream_async_id = 200000;

int32_t* StreamState() {
  return UbiGetStreamBaseState();
}

void SetState(int index, int32_t value) {
  int32_t* state = StreamState();
  if (state == nullptr) return;
  state[index] = value;
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

napi_value MakeDouble(napi_env env, double value) {
  napi_value out = nullptr;
  napi_create_double(env, value, &out);
  return out;
}

napi_value Undefined(napi_env env) {
  napi_value out = nullptr;
  napi_get_undefined(env, &out);
  return out;
}

bool GetThisAndWrap(napi_env env,
                    napi_callback_info info,
                    size_t* argc_out,
                    napi_value* argv,
                    napi_value* self_out,
                    JsStreamWrap** wrap_out) {
  size_t argc = argc_out != nullptr ? *argc_out : 0;
  napi_value self = nullptr;
  if (napi_get_cb_info(env, info, &argc, argv, &self, nullptr) != napi_ok || self == nullptr) return false;
  if (argc_out != nullptr) *argc_out = argc;
  if (self_out != nullptr) *self_out = self;
  if (wrap_out != nullptr) {
    *wrap_out = nullptr;
    napi_unwrap(env, self, reinterpret_cast<void**>(wrap_out));
  }
  return true;
}

std::string ValueToUtf8(napi_env env, napi_value value) {
  if (value == nullptr) return {};
  if (napi_coerce_to_string(env, value, &value) != napi_ok || value == nullptr) return {};
  size_t len = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &len) != napi_ok) return {};
  std::string out(len + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, value, out.data(), out.size(), &copied) != napi_ok) return {};
  out.resize(copied);
  return out;
}

bool ExtractByteSpan(napi_env env,
                     napi_value value,
                     const uint8_t** data_out,
                     size_t* len_out,
                     std::string* temp_utf8) {
  if (data_out == nullptr || len_out == nullptr || temp_utf8 == nullptr) return false;
  *data_out = nullptr;
  *len_out = 0;
  temp_utf8->clear();
  if (value == nullptr) return true;

  bool is_buffer = false;
  if (napi_is_buffer(env, value, &is_buffer) == napi_ok && is_buffer) {
    void* data = nullptr;
    size_t len = 0;
    if (napi_get_buffer_info(env, value, &data, &len) == napi_ok) {
      *data_out = static_cast<const uint8_t*>(data);
      *len_out = len;
      return true;
    }
  }

  bool is_typed = false;
  if (napi_is_typedarray(env, value, &is_typed) == napi_ok && is_typed) {
    napi_typedarray_type ta_type;
    size_t length = 0;
    void* data = nullptr;
    napi_value arraybuffer = nullptr;
    size_t byte_offset = 0;
    if (napi_get_typedarray_info(env,
                                 value,
                                 &ta_type,
                                 &length,
                                 &data,
                                 &arraybuffer,
                                 &byte_offset) == napi_ok &&
        data != nullptr) {
      *data_out = static_cast<const uint8_t*>(data);
      *len_out = length;
      return true;
    }
  }

  bool is_ab = false;
  if (napi_is_arraybuffer(env, value, &is_ab) == napi_ok && is_ab) {
    void* data = nullptr;
    size_t len = 0;
    if (napi_get_arraybuffer_info(env, value, &data, &len) == napi_ok && data != nullptr) {
      *data_out = static_cast<const uint8_t*>(data);
      *len_out = len;
      return true;
    }
  }

  *temp_utf8 = ValueToUtf8(env, value);
  *data_out = reinterpret_cast<const uint8_t*>(temp_utf8->data());
  *len_out = temp_utf8->size();
  return true;
}

bool IsFunction(napi_env env, napi_value value) {
  if (value == nullptr) return false;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok) return false;
  return type == napi_function;
}

int32_t CallMethodReturningInt32(napi_env env,
                                 napi_value self,
                                 const char* method_name,
                                 size_t argc,
                                 napi_value* argv,
                                 int32_t fallback) {
  if (self == nullptr || method_name == nullptr) return fallback;
  napi_value fn = nullptr;
  if (napi_get_named_property(env, self, method_name, &fn) != napi_ok || !IsFunction(env, fn)) {
    return fallback;
  }
  napi_value result = nullptr;
  if (UbiMakeCallback(env, self, fn, argc, argv, &result) != napi_ok || result == nullptr) {
    return fallback;
  }
  int32_t out = fallback;
  if (napi_get_value_int32(env, result, &out) != napi_ok) return fallback;
  return out;
}

void InvokeOnRead(napi_env env, napi_value self, napi_value payload) {
  if (self == nullptr) return;
  napi_value onread = nullptr;
  if (napi_get_named_property(env, self, "onread", &onread) != napi_ok || !IsFunction(env, onread)) return;
  napi_value argv[1] = {payload != nullptr ? payload : Undefined(env)};
  napi_value ignored = nullptr;
  UbiMakeCallback(env, self, onread, 1, argv, &ignored);
}

napi_value CallOnWrite(napi_env env,
                       napi_value self,
                       napi_value req_obj,
                       const std::vector<std::vector<uint8_t>>& chunks,
                       size_t total_bytes) {
  napi_value array = nullptr;
  if (napi_create_array_with_length(env, chunks.size(), &array) != napi_ok || array == nullptr) {
    SetState(kUbiBytesWritten, 0);
    SetState(kUbiLastWriteWasAsync, 0);
    return MakeInt32(env, UV_EPROTO);
  }

  for (size_t i = 0; i < chunks.size(); ++i) {
    napi_value buf = nullptr;
    void* out = nullptr;
    const auto& chunk = chunks[i];
    if (napi_create_buffer_copy(env, chunk.size(), chunk.data(), &out, &buf) != napi_ok || buf == nullptr) {
      SetState(kUbiBytesWritten, 0);
      SetState(kUbiLastWriteWasAsync, 0);
      return MakeInt32(env, UV_EPROTO);
    }
    napi_set_element(env, array, static_cast<uint32_t>(i), buf);
  }

  napi_value argv[2] = {req_obj != nullptr ? req_obj : Undefined(env), array};
  const int32_t status = CallMethodReturningInt32(env, self, "onwrite", 2, argv, UV_EPROTO);
  if (status == 0) {
    SetState(kUbiBytesWritten, static_cast<int32_t>(total_bytes));
    SetState(kUbiLastWriteWasAsync, 1);
  } else {
    SetState(kUbiBytesWritten, 0);
    SetState(kUbiLastWriteWasAsync, 0);
  }
  return MakeInt32(env, status);
}

void JsStreamFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* wrap = static_cast<JsStreamWrap*>(data);
  if (wrap == nullptr) return;
  if (wrap->wrapper_ref != nullptr) {
    napi_delete_reference(env, wrap->wrapper_ref);
    wrap->wrapper_ref = nullptr;
  }
  delete wrap;
}

napi_value JsStreamCtor(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  if (self == nullptr) return Undefined(env);

  auto* wrap = new JsStreamWrap();
  wrap->env = env;
  wrap->async_id = g_next_js_stream_async_id++;
  napi_wrap(env, self, wrap, JsStreamFinalize, nullptr, &wrap->wrapper_ref);

  napi_value false_value = nullptr;
  napi_get_boolean(env, false, &false_value);
  if (false_value != nullptr) napi_set_named_property(env, self, "reading", false_value);

  napi_value true_value = nullptr;
  napi_get_boolean(env, true, &true_value);
  if (true_value != nullptr) napi_set_named_property(env, self, "isStreamBase", true_value);

  return self;
}

napi_value JsStreamReadStart(napi_env env, napi_callback_info info) {
  JsStreamWrap* wrap = nullptr;
  napi_value self = nullptr;
  if (!GetThisAndWrap(env, info, nullptr, nullptr, &self, &wrap) || self == nullptr) {
    return MakeInt32(env, UV_EBADF);
  }
  const int32_t status = CallMethodReturningInt32(env, self, "onreadstart", 0, nullptr, UV_EPROTO);
  napi_value reading = nullptr;
  napi_get_boolean(env, status == 0, &reading);
  if (reading != nullptr) napi_set_named_property(env, self, "reading", reading);
  return MakeInt32(env, status);
}

napi_value JsStreamReadStop(napi_env env, napi_callback_info info) {
  JsStreamWrap* wrap = nullptr;
  napi_value self = nullptr;
  if (!GetThisAndWrap(env, info, nullptr, nullptr, &self, &wrap) || self == nullptr) {
    return MakeInt32(env, UV_EBADF);
  }
  const int32_t status = CallMethodReturningInt32(env, self, "onreadstop", 0, nullptr, UV_EPROTO);
  napi_value reading = nullptr;
  napi_get_boolean(env, false, &reading);
  if (reading != nullptr) napi_set_named_property(env, self, "reading", reading);
  return MakeInt32(env, status);
}

napi_value JsStreamShutdown(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  JsStreamWrap* wrap = nullptr;
  if (!GetThisAndWrap(env, info, &argc, argv, &self, &wrap) || self == nullptr) {
    return MakeInt32(env, UV_EBADF);
  }
  napi_value req_obj = argc >= 1 ? argv[0] : Undefined(env);
  napi_value cb_argv[1] = {req_obj};
  const int32_t status = CallMethodReturningInt32(env, self, "onshutdown", 1, cb_argv, UV_EPROTO);
  return MakeInt32(env, status);
}

napi_value JsStreamWriteBuffer(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_value self = nullptr;
  JsStreamWrap* wrap = nullptr;
  if (!GetThisAndWrap(env, info, &argc, argv, &self, &wrap) || self == nullptr || wrap == nullptr || argc < 2) {
    return MakeInt32(env, UV_EINVAL);
  }

  const uint8_t* data = nullptr;
  size_t len = 0;
  std::string temp;
  ExtractByteSpan(env, argv[1], &data, &len, &temp);
  std::vector<std::vector<uint8_t>> chunks;
  chunks.emplace_back(data, data + len);
  napi_value status = CallOnWrite(env, self, argv[0], chunks, len);
  int32_t status_i = UV_EPROTO;
  napi_get_value_int32(env, status, &status_i);
  if (status_i == 0) wrap->bytes_written += len;
  return status;
}

napi_value JsStreamWriteString(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_value self = nullptr;
  JsStreamWrap* wrap = nullptr;
  if (!GetThisAndWrap(env, info, &argc, argv, &self, &wrap) || self == nullptr || wrap == nullptr || argc < 2) {
    return MakeInt32(env, UV_EINVAL);
  }

  std::string s = ValueToUtf8(env, argv[1]);
  std::vector<std::vector<uint8_t>> chunks;
  chunks.emplace_back(reinterpret_cast<const uint8_t*>(s.data()),
                      reinterpret_cast<const uint8_t*>(s.data()) + s.size());
  napi_value status = CallOnWrite(env, self, argv[0], chunks, s.size());
  int32_t status_i = UV_EPROTO;
  napi_get_value_int32(env, status, &status_i);
  if (status_i == 0) wrap->bytes_written += s.size();
  return status;
}

napi_value JsStreamWritev(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  napi_value self = nullptr;
  JsStreamWrap* wrap = nullptr;
  if (!GetThisAndWrap(env, info, &argc, argv, &self, &wrap) || self == nullptr || wrap == nullptr || argc < 2) {
    return MakeInt32(env, UV_EINVAL);
  }
  napi_value chunks_input = argv[1];
  bool all_buffers = false;
  if (argc >= 3 && argv[2] != nullptr) napi_get_value_bool(env, argv[2], &all_buffers);

  uint32_t raw_len = 0;
  if (napi_get_array_length(env, chunks_input, &raw_len) != napi_ok) return MakeInt32(env, UV_EINVAL);
  const uint32_t n = all_buffers ? raw_len : (raw_len / 2);
  if (n == 0) {
    SetState(kUbiBytesWritten, 0);
    SetState(kUbiLastWriteWasAsync, 0);
    return MakeInt32(env, 0);
  }

  std::vector<std::vector<uint8_t>> chunks;
  chunks.reserve(n);
  size_t total = 0;
  for (uint32_t i = 0; i < n; ++i) {
    napi_value chunk_v = nullptr;
    napi_get_element(env, chunks_input, all_buffers ? i : (i * 2), &chunk_v);
    const uint8_t* data = nullptr;
    size_t len = 0;
    std::string temp;
    ExtractByteSpan(env, chunk_v, &data, &len, &temp);
    chunks.emplace_back(data, data + len);
    total += len;
  }

  napi_value status = CallOnWrite(env, self, argv[0], chunks, total);
  int32_t status_i = UV_EPROTO;
  napi_get_value_int32(env, status, &status_i);
  if (status_i == 0) wrap->bytes_written += total;
  return status;
}

napi_value JsStreamUseUserBuffer(napi_env env, napi_callback_info info) {
  (void)info;
  return Undefined(env);
}

napi_value JsStreamReadBuffer(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  JsStreamWrap* wrap = nullptr;
  if (!GetThisAndWrap(env, info, &argc, argv, &self, &wrap) || self == nullptr || wrap == nullptr || argc < 1) {
    return Undefined(env);
  }

  const uint8_t* data = nullptr;
  size_t len = 0;
  std::string temp;
  ExtractByteSpan(env, argv[0], &data, &len, &temp);

  void* out_data = nullptr;
  napi_value ab = nullptr;
  if (napi_create_arraybuffer(env, len, &out_data, &ab) != napi_ok || ab == nullptr || out_data == nullptr) {
    return Undefined(env);
  }
  if (len > 0 && data != nullptr) memcpy(out_data, data, len);
  SetState(kUbiReadBytesOrError, static_cast<int32_t>(len));
  SetState(kUbiArrayBufferOffset, 0);
  wrap->bytes_read += len;
  InvokeOnRead(env, self, ab);
  return Undefined(env);
}

napi_value JsStreamEmitEOF(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  JsStreamWrap* wrap = nullptr;
  if (!GetThisAndWrap(env, info, nullptr, nullptr, &self, &wrap) || self == nullptr) return Undefined(env);
  SetState(kUbiReadBytesOrError, UV_EOF);
  SetState(kUbiArrayBufferOffset, 0);
  InvokeOnRead(env, self, Undefined(env));
  return Undefined(env);
}

void FinishReq(napi_env env, napi_value req_obj, int32_t status) {
  if (req_obj == nullptr) return;
  napi_value oncomplete = nullptr;
  if (napi_get_named_property(env, req_obj, "oncomplete", &oncomplete) != napi_ok || !IsFunction(env, oncomplete)) {
    return;
  }
  napi_value argv[1] = {MakeInt32(env, status)};
  napi_value ignored = nullptr;
  UbiMakeCallback(env, req_obj, oncomplete, 1, argv, &ignored);
}

napi_value JsStreamFinishWrite(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 2) return Undefined(env);
  int32_t status = 0;
  if (napi_get_value_int32(env, argv[1], &status) != napi_ok) status = UV_EINVAL;
  FinishReq(env, argv[0], status);
  return Undefined(env);
}

napi_value JsStreamFinishShutdown(napi_env env, napi_callback_info info) {
  return JsStreamFinishWrite(env, info);
}

napi_value JsStreamClose(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc >= 1 && IsFunction(env, argv[0])) {
    napi_value ignored = nullptr;
    UbiMakeCallback(env, Undefined(env), argv[0], 0, nullptr, &ignored);
  }
  return Undefined(env);
}

napi_value JsStreamGetAsyncId(napi_env env, napi_callback_info info) {
  JsStreamWrap* wrap = nullptr;
  GetThisAndWrap(env, info, nullptr, nullptr, nullptr, &wrap);
  return MakeInt64(env, wrap != nullptr ? wrap->async_id : -1);
}

napi_value JsStreamAsyncReset(napi_env env, napi_callback_info info) {
  JsStreamWrap* wrap = nullptr;
  GetThisAndWrap(env, info, nullptr, nullptr, nullptr, &wrap);
  if (wrap != nullptr) wrap->async_id = g_next_js_stream_async_id++;
  return Undefined(env);
}

napi_value JsStreamGetProviderType(napi_env env, napi_callback_info info) {
  (void)info;
  return MakeInt32(env, 0);
}

napi_value JsStreamBytesReadGetter(napi_env env, napi_callback_info info) {
  JsStreamWrap* wrap = nullptr;
  GetThisAndWrap(env, info, nullptr, nullptr, nullptr, &wrap);
  return MakeDouble(env, static_cast<double>(wrap != nullptr ? wrap->bytes_read : 0));
}

napi_value JsStreamBytesWrittenGetter(napi_env env, napi_callback_info info) {
  JsStreamWrap* wrap = nullptr;
  GetThisAndWrap(env, info, nullptr, nullptr, nullptr, &wrap);
  return MakeDouble(env, static_cast<double>(wrap != nullptr ? wrap->bytes_written : 0));
}

napi_value JsStreamFdGetter(napi_env env, napi_callback_info info) {
  (void)info;
  return MakeInt32(env, -1);
}

}  // namespace

napi_value UbiInstallJsStreamBinding(napi_env env) {
  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) return nullptr;

  constexpr napi_property_attributes kMethodAttrs =
      static_cast<napi_property_attributes>(napi_writable | napi_configurable);

  napi_property_descriptor js_stream_props[] = {
      {"readStart", nullptr, JsStreamReadStart, nullptr, nullptr, nullptr, kMethodAttrs, nullptr},
      {"readStop", nullptr, JsStreamReadStop, nullptr, nullptr, nullptr, kMethodAttrs, nullptr},
      {"shutdown", nullptr, JsStreamShutdown, nullptr, nullptr, nullptr, kMethodAttrs, nullptr},
      {"useUserBuffer", nullptr, JsStreamUseUserBuffer, nullptr, nullptr, nullptr, kMethodAttrs, nullptr},
      {"writev", nullptr, JsStreamWritev, nullptr, nullptr, nullptr, kMethodAttrs, nullptr},
      {"writeBuffer", nullptr, JsStreamWriteBuffer, nullptr, nullptr, nullptr, kMethodAttrs, nullptr},
      {"writeAsciiString", nullptr, JsStreamWriteString, nullptr, nullptr, nullptr, kMethodAttrs, nullptr},
      {"writeUtf8String", nullptr, JsStreamWriteString, nullptr, nullptr, nullptr, kMethodAttrs, nullptr},
      {"writeUcs2String", nullptr, JsStreamWriteString, nullptr, nullptr, nullptr, kMethodAttrs, nullptr},
      {"writeLatin1String", nullptr, JsStreamWriteString, nullptr, nullptr, nullptr, kMethodAttrs, nullptr},
      {"finishWrite", nullptr, JsStreamFinishWrite, nullptr, nullptr, nullptr, kMethodAttrs, nullptr},
      {"finishShutdown", nullptr, JsStreamFinishShutdown, nullptr, nullptr, nullptr, kMethodAttrs, nullptr},
      {"readBuffer", nullptr, JsStreamReadBuffer, nullptr, nullptr, nullptr, kMethodAttrs, nullptr},
      {"emitEOF", nullptr, JsStreamEmitEOF, nullptr, nullptr, nullptr, kMethodAttrs, nullptr},
      {"close", nullptr, JsStreamClose, nullptr, nullptr, nullptr, kMethodAttrs, nullptr},
      {"getAsyncId", nullptr, JsStreamGetAsyncId, nullptr, nullptr, nullptr, kMethodAttrs, nullptr},
      {"getProviderType", nullptr, JsStreamGetProviderType, nullptr, nullptr, nullptr, kMethodAttrs, nullptr},
      {"asyncReset", nullptr, JsStreamAsyncReset, nullptr, nullptr, nullptr, kMethodAttrs, nullptr},
      {"bytesRead", nullptr, nullptr, JsStreamBytesReadGetter, nullptr, nullptr, napi_default, nullptr},
      {"bytesWritten", nullptr, nullptr, JsStreamBytesWrittenGetter, nullptr, nullptr, napi_default, nullptr},
      {"fd", nullptr, nullptr, JsStreamFdGetter, nullptr, nullptr, napi_default, nullptr},
  };

  napi_value ctor = nullptr;
  if (napi_define_class(env,
                        "JSStream",
                        NAPI_AUTO_LENGTH,
                        JsStreamCtor,
                        nullptr,
                        sizeof(js_stream_props) / sizeof(js_stream_props[0]),
                        js_stream_props,
                        &ctor) != napi_ok ||
      ctor == nullptr) {
    return nullptr;
  }

  napi_set_named_property(env, binding, "JSStream", ctor);
  return binding;
}
