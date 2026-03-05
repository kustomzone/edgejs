#include "internal_binding/dispatch.h"

#include <array>
#include <cstdint>
#include <string>

#include "internal_binding/helpers.h"

namespace internal_binding {

namespace {

uint32_t Crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
  static std::array<uint32_t, 256> table = []() {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int j = 0; j < 8; ++j) {
        c = (c & 1U) ? (0xEDB88320U ^ (c >> 1U)) : (c >> 1U);
      }
      t[i] = c;
    }
    return t;
  }();

  crc ^= 0xFFFFFFFFU;
  for (size_t i = 0; i < len; ++i) {
    crc = table[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8U);
  }
  return crc ^ 0xFFFFFFFFU;
}

bool ReadBinaryArg(napi_env env, napi_value value, std::string* out) {
  if (value == nullptr || out == nullptr) return false;

  bool is_buffer = false;
  if (napi_is_buffer(env, value, &is_buffer) == napi_ok && is_buffer) {
    void* data = nullptr;
    size_t len = 0;
    if (napi_get_buffer_info(env, value, &data, &len) != napi_ok || data == nullptr) return false;
    out->assign(static_cast<const char*>(data), len);
    return true;
  }

  bool is_typed_array = false;
  if (napi_is_typedarray(env, value, &is_typed_array) == napi_ok && is_typed_array) {
    napi_typedarray_type type = napi_uint8_array;
    size_t len = 0;
    void* data = nullptr;
    napi_value ab = nullptr;
    size_t byte_offset = 0;
    if (napi_get_typedarray_info(env, value, &type, &len, &data, &ab, &byte_offset) != napi_ok || data == nullptr) {
      return false;
    }

    size_t bytes = len;
    switch (type) {
      case napi_uint16_array:
      case napi_int16_array:
        bytes *= 2;
        break;
      case napi_uint32_array:
      case napi_int32_array:
      case napi_float32_array:
        bytes *= 4;
        break;
      case napi_float64_array:
      case napi_bigint64_array:
      case napi_biguint64_array:
        bytes *= 8;
        break;
      default:
        break;
    }
    out->assign(static_cast<const char*>(data), bytes);
    return true;
  }

  bool is_data_view = false;
  if (napi_is_dataview(env, value, &is_data_view) == napi_ok && is_data_view) {
    size_t len = 0;
    void* data = nullptr;
    napi_value ab = nullptr;
    size_t byte_offset = 0;
    if (napi_get_dataview_info(env, value, &len, &data, &ab, &byte_offset) != napi_ok || data == nullptr) {
      return false;
    }
    out->assign(static_cast<const char*>(data), len);
    return true;
  }

  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) == napi_ok && type == napi_string) {
    size_t len = 0;
    if (napi_get_value_string_utf8(env, value, nullptr, 0, &len) != napi_ok) return false;
    out->assign(len, '\0');
    size_t written = 0;
    if (napi_get_value_string_utf8(env, value, out->data(), out->size() + 1, &written) != napi_ok) return false;
    out->resize(written);
    return true;
  }

  return false;
}

napi_value ZlibCrc32(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string bytes;
  if (argc < 1 || !ReadBinaryArg(env, argv[0], &bytes)) return Undefined(env);

  uint32_t initial = 0;
  if (argc >= 2 && argv[1] != nullptr) {
    napi_get_value_uint32(env, argv[1], &initial);
  }
  const uint32_t result = Crc32Update(initial, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
  napi_value out = nullptr;
  napi_create_uint32(env, result, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value ZlibCtor(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  return this_arg != nullptr ? this_arg : Undefined(env);
}

napi_value ZlibMethodNoop(napi_env env, napi_callback_info /*info*/) {
  return Undefined(env);
}

napi_value ZlibMethodZero(napi_env env, napi_callback_info /*info*/) {
  napi_value out = nullptr;
  napi_create_int32(env, 0, &out);
  return out != nullptr ? out : Undefined(env);
}

}  // namespace

napi_value ResolveZlib(napi_env env, const ResolveOptions& /*options*/) {
  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) return Undefined(env);

  napi_value crc32 = nullptr;
  if (napi_create_function(env, "crc32", NAPI_AUTO_LENGTH, ZlibCrc32, nullptr, &crc32) == napi_ok &&
      crc32 != nullptr) {
    napi_set_named_property(env, out, "crc32", crc32);
  }

  napi_property_descriptor zlib_methods[] = {
      {"init", nullptr, ZlibMethodZero, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"params", nullptr, ZlibMethodZero, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"reset", nullptr, ZlibMethodNoop, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"close", nullptr, ZlibMethodNoop, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"write", nullptr, ZlibMethodZero, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"writeSync", nullptr, ZlibMethodZero, nullptr, nullptr, nullptr, napi_default, nullptr},
  };
  napi_value zlib_ctor = nullptr;
  if (napi_define_class(env,
                        "Zlib",
                        NAPI_AUTO_LENGTH,
                        ZlibCtor,
                        nullptr,
                        sizeof(zlib_methods) / sizeof(zlib_methods[0]),
                        zlib_methods,
                        &zlib_ctor) == napi_ok &&
      zlib_ctor != nullptr) {
    napi_set_named_property(env, out, "Zlib", zlib_ctor);
  }

  return out;
}

}  // namespace internal_binding

