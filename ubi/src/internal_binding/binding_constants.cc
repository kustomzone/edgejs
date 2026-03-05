#include "internal_binding/dispatch.h"

#include <unordered_map>

#include <openssl/ec.h>
#include <openssl/rsa.h>

#include "internal_binding/helpers.h"

namespace internal_binding {

namespace {

std::unordered_map<napi_env, napi_ref> g_constants_refs;

bool IsObjectLike(napi_env env, napi_value value) {
  if (value == nullptr) return false;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok) return false;
  return type == napi_object || type == napi_function;
}

napi_value GetNamedProperty(napi_env env, napi_value target, const char* key) {
  if (target == nullptr) return nullptr;
  napi_value out = nullptr;
  if (napi_get_named_property(env, target, key, &out) != napi_ok || out == nullptr) return nullptr;
  return out;
}

napi_value CreateNullPrototypeObject(napi_env env) {
  const napi_value undefined = Undefined(env);
  const napi_value global = GetGlobal(env);
  if (global == nullptr) return undefined;

  napi_value object_ctor = nullptr;
  if (napi_get_named_property(env, global, "Object", &object_ctor) != napi_ok ||
      object_ctor == nullptr || !IsObjectLike(env, object_ctor)) {
    return undefined;
  }

  napi_value create_fn = nullptr;
  if (napi_get_named_property(env, object_ctor, "create", &create_fn) != napi_ok ||
      create_fn == nullptr) {
    return undefined;
  }
  napi_valuetype fn_type = napi_undefined;
  if (napi_typeof(env, create_fn, &fn_type) != napi_ok || fn_type != napi_function) {
    return undefined;
  }

  napi_value null_value = nullptr;
  if (napi_get_null(env, &null_value) != napi_ok || null_value == nullptr) return undefined;

  napi_value out = nullptr;
  napi_value argv[1] = {null_value};
  if (napi_call_function(env, object_ctor, create_fn, 1, argv, &out) != napi_ok || out == nullptr) {
    bool pending = false;
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
      napi_value ignored = nullptr;
      napi_get_and_clear_last_exception(env, &ignored);
    }
    return undefined;
  }
  return out;
}

napi_value CreatePlainObject(napi_env env) {
  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) return Undefined(env);
  return out;
}

napi_value CreateBestEffortNullProtoObject(napi_env env) {
  napi_value out = CreateNullPrototypeObject(env);
  if (out == nullptr || IsUndefined(env, out) || !IsObjectLike(env, out)) {
    out = CreatePlainObject(env);
  }
  return out;
}

napi_value EnsureObjectProperty(napi_env env, napi_value target, const char* key) {
  napi_value current = GetNamedProperty(env, target, key);
  if (!IsObjectLike(env, current)) {
    current = CreatePlainObject(env);
    if (IsObjectLike(env, current)) napi_set_named_property(env, target, key, current);
  }
  return current;
}

void EnsureInt32Default(napi_env env, napi_value target, const char* key, int32_t value) {
  if (!IsObjectLike(env, target)) return;
  bool has_key = false;
  if (napi_has_named_property(env, target, key, &has_key) != napi_ok || has_key) return;
  SetInt32(env, target, key, value);
}

void CopyOwnProperties(napi_env env, napi_value src, napi_value dst) {
  if (!IsObjectLike(env, src) || !IsObjectLike(env, dst)) return;
  napi_value keys = nullptr;
  if (napi_get_property_names(env, src, &keys) != napi_ok || keys == nullptr) return;
  uint32_t key_count = 0;
  if (napi_get_array_length(env, keys, &key_count) != napi_ok) return;
  for (uint32_t i = 0; i < key_count; i++) {
    napi_value key = nullptr;
    if (napi_get_element(env, keys, i, &key) != napi_ok || key == nullptr) continue;
    napi_value value = nullptr;
    if (napi_get_property(env, src, key, &value) != napi_ok || value == nullptr) continue;
    napi_set_property(env, dst, key, value);
  }
}

bool CopyNumericOwnProperties(napi_env env, napi_value src, napi_value dst) {
  napi_value keys = nullptr;
  if (napi_get_property_names(env, src, &keys) != napi_ok || keys == nullptr) return false;
  uint32_t key_count = 0;
  if (napi_get_array_length(env, keys, &key_count) != napi_ok) return false;
  for (uint32_t i = 0; i < key_count; i++) {
    napi_value key = nullptr;
    if (napi_get_element(env, keys, i, &key) != napi_ok || key == nullptr) continue;

    napi_value value = nullptr;
    if (napi_get_property(env, src, key, &value) != napi_ok || value == nullptr) continue;

    napi_valuetype t = napi_undefined;
    if (napi_typeof(env, value, &t) != napi_ok || t != napi_number) continue;

    napi_set_property(env, dst, key, value);
  }
  return true;
}

napi_value CreateInternalConstants(napi_env env) {
  napi_value internal = nullptr;
  if (napi_create_object(env, &internal) != napi_ok || internal == nullptr) return Undefined(env);
  SetInt32(env, internal, "EXTENSIONLESS_FORMAT_JAVASCRIPT", 0);
  SetInt32(env, internal, "EXTENSIONLESS_FORMAT_WASM", 1);
  return internal;
}

napi_value CreateTraceConstants(napi_env env) {
  napi_value trace = nullptr;
  if (napi_create_object(env, &trace) != napi_ok || trace == nullptr) return Undefined(env);
  SetInt32(env, trace, "TRACE_EVENT_PHASE_NESTABLE_ASYNC_BEGIN", 'b');
  SetInt32(env, trace, "TRACE_EVENT_PHASE_NESTABLE_ASYNC_END", 'e');
  return trace;
}

napi_value CreateDefaultFsConstants(napi_env env) {
  napi_value fs_obj = CreatePlainObject(env);
  if (!IsObjectLike(env, fs_obj)) return Undefined(env);
  SetInt32(env, fs_obj, "F_OK", 0);
  SetInt32(env, fs_obj, "R_OK", 4);
  SetInt32(env, fs_obj, "W_OK", 2);
  SetInt32(env, fs_obj, "X_OK", 1);
  return fs_obj;
}

napi_value CreateEmptyObject(napi_env env) {
  return CreatePlainObject(env);
}

napi_value CreateDefaultOsConstants(napi_env env) {
  napi_value os_obj = CreatePlainObject(env);
  if (!IsObjectLike(env, os_obj)) return Undefined(env);
  napi_value signals = CreateBestEffortNullProtoObject(env);
  if (IsObjectLike(env, signals)) {
    napi_set_named_property(env, os_obj, "signals", signals);
  }
  return os_obj;
}

void SetNamedObjectIfValid(napi_env env, napi_value target, const char* key, napi_value value) {
  if (value != nullptr && !IsUndefined(env, value) && IsObjectLike(env, value)) {
    napi_set_named_property(env, target, key, value);
  }
}

void NormalizeConstantsShape(napi_env env, napi_value constants) {
  if (!IsObjectLike(env, constants)) return;

  // Ensure fs access constants are always available.
  napi_value fs_obj = EnsureObjectProperty(env, constants, "fs");
  EnsureInt32Default(env, fs_obj, "F_OK", 0);
  EnsureInt32Default(env, fs_obj, "R_OK", 4);
  EnsureInt32Default(env, fs_obj, "W_OK", 2);
  EnsureInt32Default(env, fs_obj, "X_OK", 1);

  // Keep os.signals as a clean map-like object.
  napi_value os_obj = EnsureObjectProperty(env, constants, "os");
  napi_value src_signals = GetNamedProperty(env, os_obj, "signals");
  if (!IsObjectLike(env, src_signals)) src_signals = CreatePlainObject(env);

  napi_value normalized_signals = CreateBestEffortNullProtoObject(env);
  if (!IsObjectLike(env, normalized_signals)) return;
  CopyOwnProperties(env, src_signals, normalized_signals);
  napi_object_freeze(env, normalized_signals);
  napi_set_named_property(env, os_obj, "signals", normalized_signals);

  // Keep zlib constants minimally non-empty so zlib.js can compute
  // parameter-array bounds during module initialization.
  napi_value zlib_obj = EnsureObjectProperty(env, constants, "zlib");
  EnsureInt32Default(env, zlib_obj, "Z_NO_FLUSH", 0);
  EnsureInt32Default(env, zlib_obj, "Z_PARTIAL_FLUSH", 1);
  EnsureInt32Default(env, zlib_obj, "Z_SYNC_FLUSH", 2);
  EnsureInt32Default(env, zlib_obj, "Z_FULL_FLUSH", 3);
  EnsureInt32Default(env, zlib_obj, "Z_FINISH", 4);
  EnsureInt32Default(env, zlib_obj, "Z_BLOCK", 5);
  EnsureInt32Default(env, zlib_obj, "Z_OK", 0);
  EnsureInt32Default(env, zlib_obj, "Z_STREAM_END", 1);
  EnsureInt32Default(env, zlib_obj, "Z_NEED_DICT", 2);
  EnsureInt32Default(env, zlib_obj, "Z_ERRNO", -1);
  EnsureInt32Default(env, zlib_obj, "Z_STREAM_ERROR", -2);
  EnsureInt32Default(env, zlib_obj, "Z_DATA_ERROR", -3);
  EnsureInt32Default(env, zlib_obj, "Z_MEM_ERROR", -4);
  EnsureInt32Default(env, zlib_obj, "Z_BUF_ERROR", -5);
  EnsureInt32Default(env, zlib_obj, "Z_VERSION_ERROR", -6);
  EnsureInt32Default(env, zlib_obj, "Z_NO_COMPRESSION", 0);
  EnsureInt32Default(env, zlib_obj, "Z_BEST_SPEED", 1);
  EnsureInt32Default(env, zlib_obj, "Z_BEST_COMPRESSION", 9);
  EnsureInt32Default(env, zlib_obj, "Z_DEFAULT_COMPRESSION", -1);
  EnsureInt32Default(env, zlib_obj, "Z_DEFAULT_LEVEL", -1);
  EnsureInt32Default(env, zlib_obj, "Z_FILTERED", 1);
  EnsureInt32Default(env, zlib_obj, "Z_HUFFMAN_ONLY", 2);
  EnsureInt32Default(env, zlib_obj, "Z_RLE", 3);
  EnsureInt32Default(env, zlib_obj, "Z_FIXED", 4);
  EnsureInt32Default(env, zlib_obj, "Z_DEFAULT_STRATEGY", 0);
  EnsureInt32Default(env, zlib_obj, "DEFLATE", 1);
  EnsureInt32Default(env, zlib_obj, "INFLATE", 2);
  EnsureInt32Default(env, zlib_obj, "GZIP", 3);
  EnsureInt32Default(env, zlib_obj, "GUNZIP", 4);
  EnsureInt32Default(env, zlib_obj, "DEFLATERAW", 5);
  EnsureInt32Default(env, zlib_obj, "INFLATERAW", 6);
  EnsureInt32Default(env, zlib_obj, "UNZIP", 7);
  EnsureInt32Default(env, zlib_obj, "BROTLI_DECODE", 8);
  EnsureInt32Default(env, zlib_obj, "BROTLI_ENCODE", 9);
  EnsureInt32Default(env, zlib_obj, "ZSTD_COMPRESS", 10);
  EnsureInt32Default(env, zlib_obj, "ZSTD_DECOMPRESS", 11);
  EnsureInt32Default(env, zlib_obj, "Z_MIN_WINDOWBITS", 8);
  EnsureInt32Default(env, zlib_obj, "Z_MAX_WINDOWBITS", 15);
  EnsureInt32Default(env, zlib_obj, "Z_DEFAULT_WINDOWBITS", 15);
  EnsureInt32Default(env, zlib_obj, "Z_MIN_CHUNK", 64);
  EnsureInt32Default(env, zlib_obj, "Z_MAX_CHUNK", 2147483647);
  EnsureInt32Default(env, zlib_obj, "Z_DEFAULT_CHUNK", 16384);
  EnsureInt32Default(env, zlib_obj, "Z_MIN_MEMLEVEL", 1);
  EnsureInt32Default(env, zlib_obj, "Z_MAX_MEMLEVEL", 9);
  EnsureInt32Default(env, zlib_obj, "Z_DEFAULT_MEMLEVEL", 8);
  EnsureInt32Default(env, zlib_obj, "Z_MIN_LEVEL", -1);
  EnsureInt32Default(env, zlib_obj, "Z_MAX_LEVEL", 9);
  EnsureInt32Default(env, zlib_obj, "BROTLI_OPERATION_PROCESS", 0);
  EnsureInt32Default(env, zlib_obj, "BROTLI_OPERATION_FLUSH", 1);
  EnsureInt32Default(env, zlib_obj, "BROTLI_OPERATION_FINISH", 2);
  EnsureInt32Default(env, zlib_obj, "BROTLI_OPERATION_EMIT_METADATA", 3);
  EnsureInt32Default(env, zlib_obj, "BROTLI_PARAM_QUALITY", 1);
  EnsureInt32Default(env, zlib_obj, "BROTLI_PARAM_MODE", 0);
  EnsureInt32Default(env, zlib_obj, "BROTLI_PARAM_LGWIN", 2);
  EnsureInt32Default(env, zlib_obj, "BROTLI_PARAM_LGBLOCK", 3);
  EnsureInt32Default(env, zlib_obj, "BROTLI_PARAM_DISABLE_LITERAL_CONTEXT_MODELING", 4);
  EnsureInt32Default(env, zlib_obj, "BROTLI_PARAM_SIZE_HINT", 5);
  EnsureInt32Default(env, zlib_obj, "BROTLI_PARAM_LARGE_WINDOW", 6);
  EnsureInt32Default(env, zlib_obj, "BROTLI_PARAM_NPOSTFIX", 7);
  EnsureInt32Default(env, zlib_obj, "BROTLI_PARAM_NDIRECT", 8);
  EnsureInt32Default(env, zlib_obj, "ZSTD_e_continue", 0);
  EnsureInt32Default(env, zlib_obj, "ZSTD_e_flush", 1);
  EnsureInt32Default(env, zlib_obj, "ZSTD_e_end", 2);
  EnsureInt32Default(env, zlib_obj, "ZSTD_c_compressionLevel", 1);
  EnsureInt32Default(env, zlib_obj, "ZSTD_d_windowLogMax", 1);

  // Keep critical crypto constants aligned with Node's constants module shape.
  napi_value crypto_obj = EnsureObjectProperty(env, constants, "crypto");
#ifdef RSA_PKCS1_PADDING
  EnsureInt32Default(env, crypto_obj, "RSA_PKCS1_PADDING", RSA_PKCS1_PADDING);
#endif
#ifdef RSA_NO_PADDING
  EnsureInt32Default(env, crypto_obj, "RSA_NO_PADDING", RSA_NO_PADDING);
#endif
#ifdef RSA_PKCS1_OAEP_PADDING
  EnsureInt32Default(env, crypto_obj, "RSA_PKCS1_OAEP_PADDING", RSA_PKCS1_OAEP_PADDING);
#endif
#ifdef RSA_X931_PADDING
  EnsureInt32Default(env, crypto_obj, "RSA_X931_PADDING", RSA_X931_PADDING);
#endif
#ifdef RSA_PKCS1_PSS_PADDING
  EnsureInt32Default(env, crypto_obj, "RSA_PKCS1_PSS_PADDING", RSA_PKCS1_PSS_PADDING);
#endif
#ifdef RSA_PSS_SALTLEN_DIGEST
  EnsureInt32Default(env, crypto_obj, "RSA_PSS_SALTLEN_DIGEST", RSA_PSS_SALTLEN_DIGEST);
#endif
#ifdef RSA_PSS_SALTLEN_MAX_SIGN
  EnsureInt32Default(env, crypto_obj, "RSA_PSS_SALTLEN_MAX_SIGN", RSA_PSS_SALTLEN_MAX_SIGN);
#endif
#ifdef RSA_PSS_SALTLEN_AUTO
  EnsureInt32Default(env, crypto_obj, "RSA_PSS_SALTLEN_AUTO", RSA_PSS_SALTLEN_AUTO);
#endif
#ifdef POINT_CONVERSION_COMPRESSED
  EnsureInt32Default(env, crypto_obj, "POINT_CONVERSION_COMPRESSED", POINT_CONVERSION_COMPRESSED);
#endif
#ifdef POINT_CONVERSION_UNCOMPRESSED
  EnsureInt32Default(env, crypto_obj, "POINT_CONVERSION_UNCOMPRESSED", POINT_CONVERSION_UNCOMPRESSED);
#endif
#ifdef POINT_CONVERSION_HYBRID
  EnsureInt32Default(env, crypto_obj, "POINT_CONVERSION_HYBRID", POINT_CONVERSION_HYBRID);
#endif
}

}  // namespace

napi_value ResolveConstants(napi_env env, const ResolveOptions& options) {
  const napi_value undefined = Undefined(env);
  auto cached_it = g_constants_refs.find(env);
  if (cached_it != g_constants_refs.end() && cached_it->second != nullptr) {
    napi_value cached = nullptr;
    if (napi_get_reference_value(env, cached_it->second, &cached) == napi_ok && cached != nullptr) {
      return cached;
    }
  }

  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) return undefined;

  SetNamedObjectIfValid(env, out, "os", CreateDefaultOsConstants(env));
  SetNamedObjectIfValid(env, out, "fs", CreateDefaultFsConstants(env));
  SetNamedObjectIfValid(env, out, "crypto", CreateEmptyObject(env));
  SetNamedObjectIfValid(env, out, "zlib", CreateEmptyObject(env));

  // Prefer native ubi constants when present.
  napi_value os_constants = nullptr;
  if (options.callbacks.resolve_binding != nullptr) {
    os_constants = options.callbacks.resolve_binding(env, options.state, "os_constants");
  }
  SetNamedObjectIfValid(env, out, "os", os_constants);

  napi_value fs_binding = nullptr;
  if (options.callbacks.resolve_binding != nullptr) {
    fs_binding = options.callbacks.resolve_binding(env, options.state, "fs");
  }
  if (!IsUndefined(env, fs_binding) && IsObjectLike(env, fs_binding)) {
    napi_value fs_constants_obj = nullptr;
    if (napi_create_object(env, &fs_constants_obj) == napi_ok && fs_constants_obj != nullptr) {
      CopyNumericOwnProperties(env, fs_binding, fs_constants_obj);
      SetNamedObjectIfValid(env, out, "fs", fs_constants_obj);
    }
  }

  // Derive crypto constants from the native crypto binding surface to avoid
  // requiring JS modules while constants are initializing.
  napi_value crypto_binding = nullptr;
  if (options.callbacks.resolve_binding != nullptr) {
    crypto_binding = options.callbacks.resolve_binding(env, options.state, "crypto");
  }
  if (!IsUndefined(env, crypto_binding) && IsObjectLike(env, crypto_binding)) {
    napi_value crypto_constants_obj = nullptr;
    if (napi_create_object(env, &crypto_constants_obj) == napi_ok && crypto_constants_obj != nullptr) {
      CopyNumericOwnProperties(env, crypto_binding, crypto_constants_obj);
      SetNamedObjectIfValid(env, out, "crypto", crypto_constants_obj);
    }
  }

  SetNamedObjectIfValid(env, out, "internal", CreateInternalConstants(env));
  SetNamedObjectIfValid(env, out, "trace", CreateTraceConstants(env));
  NormalizeConstantsShape(env, out);

  auto& ref = g_constants_refs[env];
  if (ref != nullptr) {
    napi_delete_reference(env, ref);
    ref = nullptr;
  }
  napi_create_reference(env, out, 1, &ref);

  return out;
}

}  // namespace internal_binding
