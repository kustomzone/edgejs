#include "crypto/ubi_crypto_binding.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "ncrypto.h"
#include <openssl/crypto.h>
#include <openssl/core_names.h>
#include <openssl/dsa.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ecdsa.h>
#include <openssl/params.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace ubi::crypto {
namespace {

std::string ValueToUtf8(napi_env env, napi_value value) {
  size_t len = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &len) != napi_ok) return "";
  std::string out(len + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, value, out.data(), out.size(), &copied) != napi_ok) return "";
  out.resize(copied);
  return out;
}

size_t TypedArrayBytesPerElement(napi_typedarray_type type);

bool GetBufferBytes(napi_env env, napi_value value, uint8_t** data, size_t* len) {
  static uint8_t kEmptyBufferSentinel = 0;
  if (value == nullptr || data == nullptr || len == nullptr) return false;

  bool is_buffer = false;
  if (napi_is_buffer(env, value, &is_buffer) == napi_ok && is_buffer) {
    if (napi_get_buffer_info(env, value, reinterpret_cast<void**>(data), len) != napi_ok) {
      return false;
    }
    if (*len == 0 && *data == nullptr) {
      *data = &kEmptyBufferSentinel;
    }
    return true;
  }

  bool is_arraybuffer = false;
  if (napi_is_arraybuffer(env, value, &is_arraybuffer) == napi_ok && is_arraybuffer) {
    void* raw = nullptr;
    size_t byte_len = 0;
    if (napi_get_arraybuffer_info(env, value, &raw, &byte_len) != napi_ok) return false;
    if (raw == nullptr && byte_len != 0) return false;
    *data = raw != nullptr ? static_cast<uint8_t*>(raw) : &kEmptyBufferSentinel;
    *len = byte_len;
    return true;
  }

  bool is_typedarray = false;
  if (napi_is_typedarray(env, value, &is_typedarray) == napi_ok && is_typedarray) {
    napi_typedarray_type ta_type = napi_uint8_array;
    size_t element_len = 0;
    void* raw = nullptr;
    napi_value ab = nullptr;
    size_t byte_offset = 0;
    if (napi_get_typedarray_info(env, value, &ta_type, &element_len, &raw, &ab, &byte_offset) != napi_ok) {
      return false;
    }
    const size_t byte_len = element_len * TypedArrayBytesPerElement(ta_type);
    if (raw == nullptr && byte_len != 0) return false;
    *data = raw != nullptr ? static_cast<uint8_t*>(raw) : &kEmptyBufferSentinel;
    *len = byte_len;
    return true;
  }

  bool is_dataview = false;
  if (napi_is_dataview(env, value, &is_dataview) == napi_ok && is_dataview) {
    size_t byte_len = 0;
    void* raw = nullptr;
    napi_value ab = nullptr;
    size_t byte_offset = 0;
    if (napi_get_dataview_info(env, value, &byte_len, &raw, &ab, &byte_offset) != napi_ok) return false;
    if (raw == nullptr && byte_len != 0) return false;
    *data = raw != nullptr ? static_cast<uint8_t*>(raw) : &kEmptyBufferSentinel;
    *len = byte_len;
    return true;
  }

  return false;
}

bool GetBufferOrStringBytes(napi_env env,
                            napi_value value,
                            std::vector<uint8_t>* owned,
                            uint8_t** data,
                            size_t* len) {
  static uint8_t kEmptyBufferSentinel = 0;
  if (owned == nullptr || data == nullptr || len == nullptr) return false;
  if (GetBufferBytes(env, value, data, len)) return true;

  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok || type != napi_string) return false;
  std::string text = ValueToUtf8(env, value);
  owned->assign(text.begin(), text.end());
  if (owned->empty()) {
    *data = &kEmptyBufferSentinel;
    *len = 0;
  } else {
    *data = owned->data();
    *len = owned->size();
  }
  return true;
}

bool GetKeyBytes(napi_env env,
                 napi_value value,
                 std::vector<uint8_t>* owned,
                 uint8_t** data,
                 size_t* len) {
  if (GetBufferOrStringBytes(env, value, owned, data, len)) return true;
  if (value == nullptr) return false;

  napi_value export_fn = nullptr;
  napi_valuetype type = napi_undefined;
  if (napi_get_named_property(env, value, "export", &export_fn) != napi_ok ||
      export_fn == nullptr ||
      napi_typeof(env, export_fn, &type) != napi_ok ||
      type != napi_function) {
    return false;
  }

  napi_value exported = nullptr;
  if (napi_call_function(env, value, export_fn, 0, nullptr, &exported) != napi_ok || exported == nullptr) {
    return false;
  }
  return GetBufferOrStringBytes(env, exported, owned, data, len);
}

bool IsNullOrUndefined(napi_env env, napi_value value) {
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok) return false;
  return type == napi_undefined || type == napi_null;
}

bool ReadPassphrase(napi_env env, napi_value value, std::string* out, bool* provided) {
  if (out == nullptr || provided == nullptr) return false;
  *provided = false;
  out->clear();
  if (value == nullptr || IsNullOrUndefined(env, value)) return true;

  uint8_t* bytes = nullptr;
  size_t byte_len = 0;
  if (GetBufferBytes(env, value, &bytes, &byte_len)) {
    out->assign(reinterpret_cast<const char*>(bytes), byte_len);
    *provided = true;
    return true;
  }

  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok || type != napi_string) return false;
  *out = ValueToUtf8(env, value);
  *provided = true;
  return true;
}

size_t TypedArrayBytesPerElement(napi_typedarray_type type) {
  switch (type) {
    case napi_int8_array:
    case napi_uint8_array:
    case napi_uint8_clamped_array:
      return 1;
    case napi_int16_array:
    case napi_uint16_array:
      return 2;
    case napi_int32_array:
    case napi_uint32_array:
    case napi_float32_array:
      return 4;
    case napi_float64_array:
    case napi_bigint64_array:
    case napi_biguint64_array:
      return 8;
    default:
      return 1;
  }
}

bool GetBufferSourceBytes(napi_env env, napi_value value, uint8_t** data, size_t* len) {
  static uint8_t kEmptyBufferSentinel = 0;
  if (GetBufferBytes(env, value, data, len)) return true;

  bool is_arraybuffer = false;
  if (napi_is_arraybuffer(env, value, &is_arraybuffer) == napi_ok && is_arraybuffer) {
    void* raw = nullptr;
    size_t byte_len = 0;
    if (napi_get_arraybuffer_info(env, value, &raw, &byte_len) != napi_ok) return false;
    if (raw == nullptr && byte_len != 0) return false;
    *data = raw != nullptr ? static_cast<uint8_t*>(raw) : &kEmptyBufferSentinel;
    *len = byte_len;
    return true;
  }

  bool is_typedarray = false;
  if (napi_is_typedarray(env, value, &is_typedarray) == napi_ok && is_typedarray) {
    napi_typedarray_type ta_type = napi_uint8_array;
    size_t element_len = 0;
    void* raw = nullptr;
    napi_value ab = nullptr;
    size_t byte_offset = 0;
    if (napi_get_typedarray_info(env, value, &ta_type, &element_len, &raw, &ab, &byte_offset) != napi_ok) {
      return false;
    }
    const size_t byte_len = element_len * TypedArrayBytesPerElement(ta_type);
    if (raw == nullptr && byte_len != 0) return false;
    *data = raw != nullptr ? static_cast<uint8_t*>(raw) : &kEmptyBufferSentinel;
    *len = byte_len;
    return true;
  }

  bool is_dataview = false;
  if (napi_is_dataview(env, value, &is_dataview) == napi_ok && is_dataview) {
    size_t byte_len = 0;
    void* raw = nullptr;
    napi_value ab = nullptr;
    size_t byte_offset = 0;
    if (napi_get_dataview_info(env, value, &byte_len, &raw, &ab, &byte_offset) != napi_ok) return false;
    if (raw == nullptr && byte_len != 0) return false;
    *data = raw != nullptr ? static_cast<uint8_t*>(raw) : &kEmptyBufferSentinel;
    *len = byte_len;
    return true;
  }

  return false;
}

napi_value MakeError(napi_env env, const char* code, const char* message) {
  enum class ErrorCtorKind {
    kError,
    kTypeError,
    kRangeError,
  };
  auto classify = [](const char* err_code) -> ErrorCtorKind {
    if (err_code == nullptr) return ErrorCtorKind::kError;
    if (std::strcmp(err_code, "ERR_INVALID_ARG_TYPE") == 0 ||
        std::strcmp(err_code, "ERR_INVALID_ARG_VALUE") == 0 ||
        std::strcmp(err_code, "ERR_CRYPTO_INVALID_DIGEST") == 0 ||
        std::strcmp(err_code, "ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE") == 0) {
      return ErrorCtorKind::kTypeError;
    }
    if (std::strcmp(err_code, "ERR_OUT_OF_RANGE") == 0 ||
        std::strcmp(err_code, "ERR_CRYPTO_INVALID_KEYLEN") == 0) {
      return ErrorCtorKind::kRangeError;
    }
    return ErrorCtorKind::kError;
  };

  napi_value code_v = nullptr;
  napi_value msg_v = nullptr;
  napi_value err_v = nullptr;
  napi_create_string_utf8(env, code, NAPI_AUTO_LENGTH, &code_v);
  napi_create_string_utf8(env, message, NAPI_AUTO_LENGTH, &msg_v);
  switch (classify(code)) {
    case ErrorCtorKind::kTypeError:
      napi_create_type_error(env, code_v, msg_v, &err_v);
      break;
    case ErrorCtorKind::kRangeError:
      napi_create_range_error(env, code_v, msg_v, &err_v);
      break;
    default:
      napi_create_error(env, code_v, msg_v, &err_v);
      break;
  }
  if (err_v != nullptr && code_v != nullptr) napi_set_named_property(env, err_v, "code", code_v);
  return err_v;
}

void ThrowError(napi_env env, const char* code, const char* message);

void SetOptionalErrorStringProperty(napi_env env, napi_value err, const char* name, const char* value) {
  if (err == nullptr || name == nullptr || value == nullptr || value[0] == '\0') return;
  napi_value v = nullptr;
  if (napi_create_string_utf8(env, value, NAPI_AUTO_LENGTH, &v) != napi_ok || v == nullptr) return;

  napi_value global = nullptr;
  napi_value reflect = nullptr;
  napi_value set_fn = nullptr;
  napi_valuetype set_fn_type = napi_undefined;
  napi_value key = nullptr;
  if (napi_get_global(env, &global) == napi_ok &&
      global != nullptr &&
      napi_get_named_property(env, global, "Reflect", &reflect) == napi_ok &&
      reflect != nullptr &&
      napi_get_named_property(env, reflect, "set", &set_fn) == napi_ok &&
      set_fn != nullptr &&
      napi_typeof(env, set_fn, &set_fn_type) == napi_ok &&
      set_fn_type == napi_function &&
      napi_create_string_utf8(env, name, NAPI_AUTO_LENGTH, &key) == napi_ok &&
      key != nullptr) {
    napi_value argv[3] = {err, key, v};
    napi_value ignored = nullptr;
    if (napi_call_function(env, reflect, set_fn, 3, argv, &ignored) == napi_ok) {
      return;
    }
  }

  napi_set_named_property(env, err, name, v);
}

void ThrowOpenSslError(napi_env env, const char* code, unsigned long err, const char* fallback_message) {
  if (err == 0) {
    ThrowError(env, code, fallback_message);
    return;
  }

  const char* effective_code = code;
  const char* reason = ERR_reason_error_string(err);
  if (effective_code != nullptr &&
      std::strcmp(effective_code, "ERR_CRYPTO_OPERATION_FAILED") == 0 &&
      reason != nullptr &&
      std::strstr(reason, "illegal or unsupported padding mode") != nullptr) {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    effective_code = "ERR_OSSL_ILLEGAL_OR_UNSUPPORTED_PADDING_MODE";
#else
    effective_code = "ERR_OSSL_RSA_ILLEGAL_OR_UNSUPPORTED_PADDING_MODE";
#endif
  }
  if (effective_code != nullptr &&
      std::strcmp(effective_code, "ERR_CRYPTO_OPERATION_FAILED") == 0 &&
      reason != nullptr &&
      std::strstr(reason, "interrupted or cancelled") != nullptr) {
    effective_code = "ERR_OSSL_CRYPTO_INTERRUPTED_OR_CANCELLED";
  }

  char buf[256];
  ERR_error_string_n(err, buf, sizeof(buf));
  napi_value error = MakeError(env, effective_code, buf);
  SetOptionalErrorStringProperty(env, error, "library", ERR_lib_error_string(err));
#if OPENSSL_VERSION_NUMBER < 0x30000000L
  SetOptionalErrorStringProperty(env, error, "function", ERR_func_error_string(err));
#endif
  SetOptionalErrorStringProperty(env, error, "reason", reason);
  bool pending = false;
  if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
    return;
  }
  if (error != nullptr) {
    napi_throw(env, error);
  } else {
    ThrowError(env, code, buf);
  }
}

void ThrowError(napi_env env, const char* code, const char* message) {
  napi_value err = MakeError(env, code, message);
  if (err != nullptr) napi_throw(env, err);
}

void ThrowLastOpenSslError(napi_env env, const char* fallback_code, const char* fallback_message) {
  const unsigned long err = ERR_get_error();
  ThrowOpenSslError(env, fallback_code, err, fallback_message);
}

ncrypto::Digest ResolveDigest(const std::string& name) {
  if (name == "RSA-SHA1") return ncrypto::Digest::SHA1;
  return ncrypto::Digest::FromName(name.c_str());
}

ncrypto::Cipher ResolveCipher(const std::string& name) {
  return ncrypto::Cipher::FromName(name.c_str());
}

std::string CanonicalizeDigestName(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (char ch : in) {
    if (ch == '-') continue;
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return out;
}

struct SecureContextHolder {
  explicit SecureContextHolder(SSL_CTX* in_ctx) : ctx(in_ctx) {}
  ~SecureContextHolder() {
    if (ctx != nullptr) SSL_CTX_free(ctx);
  }
  SSL_CTX* ctx = nullptr;
};

void SecureContextFinalizer(napi_env env, void* data, void* hint) {
  (void)env;
  (void)hint;
  auto* holder = reinterpret_cast<SecureContextHolder*>(data);
  delete holder;
}

bool GetSecureContextHolder(napi_env env, napi_value value, SecureContextHolder** out) {
  if (out == nullptr) return false;
  *out = nullptr;
  void* raw = nullptr;
  if (napi_get_value_external(env, value, &raw) != napi_ok || raw == nullptr) return false;
  *out = reinterpret_cast<SecureContextHolder*>(raw);
  return true;
}

X509* ParseX509(const uint8_t* data, size_t len);
X509_CRL* ParseX509Crl(const uint8_t* data, size_t len);

napi_value CreateBufferCopy(napi_env env, const uint8_t* data, size_t len) {
  napi_value out = nullptr;
  void* copied = nullptr;
  if (napi_create_buffer_copy(env, len, len > 0 ? data : nullptr, &copied, &out) != napi_ok || out == nullptr) {
    return nullptr;
  }
  return out;
}

napi_value CreateBufferCopy(napi_env env, const ncrypto::DataPointer& dp) {
  return CreateBufferCopy(env, dp.get<uint8_t>(), dp.size());
}

napi_value CryptoHashOneShot(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 2) return nullptr;
  const std::string algo = ValueToUtf8(env, argv[0]);
  uint8_t* in = nullptr;
  size_t in_len = 0;
  if (!GetBufferBytes(env, argv[1], &in, &in_len)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "hash data must be a Buffer");
    return nullptr;
  }
  const ncrypto::Digest md = ResolveDigest(algo);
  if (!md) {
    ThrowError(env, "ERR_CRYPTO_HASH_UNKNOWN", "Unknown hash");
    return nullptr;
  }
  auto out = ncrypto::hashDigest({in, in_len}, md.get());
  if (!out) {
    const std::string canonical = CanonicalizeDigestName(algo);
    size_t default_xof_len = 0;
    if (canonical == "shake128") {
      default_xof_len = 16;
    } else if (canonical == "shake256") {
      default_xof_len = 32;
    }
    if (default_xof_len > 0) {
      out = ncrypto::xofHashDigest({in, in_len}, md.get(), default_xof_len);
    }
  }
  if (!out) {
    ThrowError(env, "ERR_CRYPTO_OPERATION_FAILED", "Hash operation failed");
    return nullptr;
  }
  return CreateBufferCopy(env, out);
}

napi_value CryptoHashOneShotXof(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 3) return nullptr;
  const std::string algo = ValueToUtf8(env, argv[0]);
  uint8_t* in = nullptr;
  size_t in_len = 0;
  if (!GetBufferBytes(env, argv[1], &in, &in_len)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "hash data must be a Buffer");
    return nullptr;
  }
  int32_t out_len_i32 = 0;
  if (napi_get_value_int32(env, argv[2], &out_len_i32) != napi_ok || out_len_i32 < 0) {
    ThrowError(env, "ERR_INVALID_ARG_VALUE", "Invalid output length");
    return nullptr;
  }
  const ncrypto::Digest md = ResolveDigest(algo);
  if (!md) {
    ThrowError(env, "ERR_CRYPTO_HASH_UNKNOWN", "Unknown hash");
    return nullptr;
  }
  auto out = ncrypto::xofHashDigest({in, in_len}, md.get(), static_cast<size_t>(out_len_i32));
  if (!out) {
    ThrowError(env, "ERR_CRYPTO_OPERATION_FAILED", "Hash operation failed");
    return nullptr;
  }
  return CreateBufferCopy(env, out);
}

napi_value CryptoHmacOneShot(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 3) return nullptr;
  const std::string algo = ValueToUtf8(env, argv[0]);
  uint8_t* key = nullptr;
  size_t key_len = 0;
  uint8_t* in = nullptr;
  size_t in_len = 0;
  if (!GetBufferBytes(env, argv[1], &key, &key_len) || !GetBufferBytes(env, argv[2], &in, &in_len)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "hmac key/data must be Buffers");
    return nullptr;
  }
  const ncrypto::Digest md = ResolveDigest(algo);
  if (!md) {
    const std::string message = "Invalid digest: " + algo;
    ThrowError(env, "ERR_CRYPTO_INVALID_DIGEST", message.c_str());
    return nullptr;
  }
  auto hmac = ncrypto::HMACCtxPointer::New();
  if (!hmac || !hmac.init({key, key_len}, md) || !hmac.update({in, in_len})) {
    ThrowError(env, "ERR_CRYPTO_OPERATION_FAILED", "HMAC operation failed");
    return nullptr;
  }
  auto out = hmac.digest();
  if (!out) {
    ThrowError(env, "ERR_CRYPTO_OPERATION_FAILED", "HMAC operation failed");
    return nullptr;
  }
  return CreateBufferCopy(env, out);
}

napi_value CryptoRandomFillSync(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) return nullptr;
  uint8_t* data = nullptr;
  size_t len = 0;
  if (!GetBufferSourceBytes(env, argv[0], &data, &len)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "buffer must be an ArrayBuffer or ArrayBufferView");
    return nullptr;
  }
  int32_t offset = 0;
  int32_t size = static_cast<int32_t>(len);
  if (argc >= 2 && argv[1] != nullptr) napi_get_value_int32(env, argv[1], &offset);
  if (argc >= 3 && argv[2] != nullptr) napi_get_value_int32(env, argv[2], &size);
  if (offset < 0 || size < 0 || static_cast<size_t>(offset + size) > len) {
    ThrowError(env, "ERR_OUT_OF_RANGE", "offset/size out of range");
    return nullptr;
  }
  if (!ncrypto::CSPRNG(data + offset, static_cast<size_t>(size))) {
    ThrowError(env, "ERR_CRYPTO_OPERATION_FAILED", "CSPRNG failed");
    return nullptr;
  }
  return argv[0];
}

napi_value CryptoRandomBytes(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) return nullptr;
  int32_t n = 0;
  if (napi_get_value_int32(env, argv[0], &n) != napi_ok || n < 0) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "size must be a number >= 0");
    return nullptr;
  }
  napi_value out = nullptr;
  napi_value ab = nullptr;
  void* out_data_raw = nullptr;
  if (napi_create_arraybuffer(env, static_cast<size_t>(n), &out_data_raw, &ab) != napi_ok || ab == nullptr) {
    return nullptr;
  }
  auto* out_data = reinterpret_cast<uint8_t*>(out_data_raw);
  if (n > 0 && !ncrypto::CSPRNG(out_data, static_cast<size_t>(n))) {
    ThrowError(env, "ERR_CRYPTO_OPERATION_FAILED", "CSPRNG failed");
    return nullptr;
  }
  if (napi_create_typedarray(env, napi_uint8_array, static_cast<size_t>(n), ab, 0, &out) != napi_ok) return nullptr;
  return out;
}

napi_value CryptoPbkdf2Sync(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value argv[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 5) return nullptr;
  uint8_t* pass = nullptr;
  size_t pass_len = 0;
  uint8_t* salt = nullptr;
  size_t salt_len = 0;
  if (!GetBufferBytes(env, argv[0], &pass, &pass_len) || !GetBufferBytes(env, argv[1], &salt, &salt_len)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "password/salt must be Buffers");
    return nullptr;
  }
  int32_t iter = 0;
  int32_t keylen = 0;
  napi_get_value_int32(env, argv[2], &iter);
  napi_get_value_int32(env, argv[3], &keylen);
  const std::string digest = ValueToUtf8(env, argv[4]);
  const ncrypto::Digest md = ResolveDigest(digest);
  if (!md) {
    const std::string message = "Invalid digest: " + digest;
    ThrowError(env, "ERR_CRYPTO_INVALID_DIGEST", message.c_str());
    return nullptr;
  }
  if (iter <= 0 || keylen < 0) {
    ThrowError(env, "ERR_INVALID_ARG_VALUE", "Invalid pbkdf2 arguments");
    return nullptr;
  }
  auto out = ncrypto::pbkdf2(md, {reinterpret_cast<char*>(pass), pass_len}, {salt, salt_len},
                             static_cast<uint32_t>(iter), static_cast<size_t>(keylen));
  if (!out) {
    ThrowError(env, "ERR_CRYPTO_OPERATION_FAILED", "PBKDF2 failed");
    return nullptr;
  }
  return CreateBufferCopy(env, out);
}

napi_value CryptoScryptSync(napi_env env, napi_callback_info info) {
  size_t argc = 7;
  napi_value argv[7] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 3) return nullptr;
  uint8_t* pass = nullptr;
  size_t pass_len = 0;
  uint8_t* salt = nullptr;
  size_t salt_len = 0;
  if (!GetBufferBytes(env, argv[0], &pass, &pass_len) || !GetBufferBytes(env, argv[1], &salt, &salt_len)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "password/salt must be Buffers");
    return nullptr;
  }
  int32_t keylen = 0;
  napi_get_value_int32(env, argv[2], &keylen);
  if (keylen < 0) {
    ThrowError(env, "ERR_INVALID_ARG_VALUE", "Invalid key length");
    return nullptr;
  }
  uint64_t N = 16384;
  uint64_t r = 8;
  uint64_t p = 1;
  uint64_t maxmem = 0;
  if (argc >= 4 && argv[3] != nullptr) {
    double v = 0;
    if (napi_get_value_double(env, argv[3], &v) == napi_ok && v > 0) N = static_cast<uint64_t>(v);
  }
  if (argc >= 5 && argv[4] != nullptr) {
    double v = 0;
    if (napi_get_value_double(env, argv[4], &v) == napi_ok && v > 0) r = static_cast<uint64_t>(v);
  }
  if (argc >= 6 && argv[5] != nullptr) {
    double v = 0;
    if (napi_get_value_double(env, argv[5], &v) == napi_ok && v > 0) p = static_cast<uint64_t>(v);
  }
  if (argc >= 7 && argv[6] != nullptr) {
    double v = 0;
    if (napi_get_value_double(env, argv[6], &v) == napi_ok && v > 0) maxmem = static_cast<uint64_t>(v);
  }
  if (!ncrypto::checkScryptParams(N, r, p, maxmem)) {
    ThrowError(env, "ERR_CRYPTO_INVALID_SCRYPT_PARAMS", "Invalid scrypt params: memory limit exceeded");
    return nullptr;
  }
  auto out = ncrypto::scrypt({reinterpret_cast<char*>(pass), pass_len}, {salt, salt_len},
                             N, r, p, maxmem, static_cast<size_t>(keylen));
  if (!out) {
    // Keep behavior aligned with previous bridge for platforms/OpenSSL builds
    // where ncrypto::scrypt can reject params that EVP_PBE_scrypt accepts.
    std::vector<uint8_t> fallback(static_cast<size_t>(keylen));
    if (EVP_PBE_scrypt(reinterpret_cast<const char*>(pass), pass_len,
                       salt, salt_len, N, r, p, maxmem, fallback.data(), fallback.size()) != 1) {
      ThrowLastOpenSslError(env, "ERR_CRYPTO_INVALID_SCRYPT_PARAMS", "Invalid scrypt params: memory limit exceeded");
      return nullptr;
    }
    return CreateBufferCopy(env, fallback.data(), fallback.size());
  }
  return CreateBufferCopy(env, out);
}

napi_value CryptoHkdfSync(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value argv[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 5) return nullptr;
  const std::string digest = ValueToUtf8(env, argv[0]);
  std::vector<uint8_t> key_owned;
  uint8_t *ikm = nullptr, *salt = nullptr, *info_bytes = nullptr;
  size_t ikm_len = 0, salt_len = 0, info_len = 0;
  if (!GetKeyBytes(env, argv[1], &key_owned, &ikm, &ikm_len) ||
      !GetBufferSourceBytes(env, argv[2], &salt, &salt_len) ||
      !GetBufferSourceBytes(env, argv[3], &info_bytes, &info_len)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "hkdf input/salt/info must be Buffers or strings");
    return nullptr;
  }
  int32_t keylen = 0;
  napi_get_value_int32(env, argv[4], &keylen);
  if (keylen < 0) {
    ThrowError(env, "ERR_INVALID_ARG_VALUE", "Invalid key length");
    return nullptr;
  }
  const ncrypto::Digest md = ResolveDigest(digest);
  if (!md) {
    const std::string message = "Invalid digest: " + digest;
    ThrowError(env, "ERR_CRYPTO_INVALID_DIGEST", message.c_str());
    return nullptr;
  }
  if (!ncrypto::checkHkdfLength(md, static_cast<size_t>(keylen))) {
    ThrowError(env, "ERR_CRYPTO_INVALID_KEYLEN", "Invalid key length");
    return nullptr;
  }
  auto out = ncrypto::hkdf(md, {ikm, ikm_len}, {info_bytes, info_len}, {salt, salt_len},
                           static_cast<size_t>(keylen));
  if (!out) {
    ThrowError(env, "ERR_CRYPTO_OPERATION_FAILED", "hkdf failed");
    return nullptr;
  }
  return CreateBufferCopy(env, out);
}

napi_value CryptoCipherTransform(napi_env env, napi_callback_info info) {
  size_t argc = 6;
  napi_value argv[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 6) return nullptr;
  const std::string algo = ValueToUtf8(env, argv[0]);
  uint8_t *key = nullptr, *iv = nullptr, *input = nullptr;
  size_t key_len = 0, iv_len = 0, in_len = 0;
  if (!GetBufferBytes(env, argv[1], &key, &key_len)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "key must be a Buffer");
    return nullptr;
  }

  bool iv_is_null = false;
  napi_valuetype iv_type = napi_undefined;
  if (napi_typeof(env, argv[2], &iv_type) == napi_ok && iv_type == napi_null) {
    iv_is_null = true;
  } else if (!GetBufferBytes(env, argv[2], &iv, &iv_len)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "iv must be a Buffer or null");
    return nullptr;
  }
  if (!GetBufferBytes(env, argv[3], &input, &in_len)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "input must be a Buffer");
    return nullptr;
  }
  bool decrypt = false;
  napi_get_value_bool(env, argv[4], &decrypt);

  const ncrypto::Cipher cipher = ResolveCipher(algo);
  if (!cipher) {
    ThrowError(env, "ERR_CRYPTO_UNKNOWN_CIPHER", "Unknown cipher");
    return nullptr;
  }
  if (cipher.getKeyLength() > 0 && key_len != static_cast<size_t>(cipher.getKeyLength())) {
    ThrowError(env, "ERR_CRYPTO_INVALID_KEYLEN", "Invalid key length");
    return nullptr;
  }
  if (!iv_is_null && cipher.getIvLength() >= 0 && iv_len != static_cast<size_t>(cipher.getIvLength())) {
    ThrowError(env, "ERR_CRYPTO_INVALID_IV", "Invalid IV length");
    return nullptr;
  }

  auto ctx = ncrypto::CipherCtxPointer::New();
  if (!ctx || !ctx.init(cipher, !decrypt, key, iv_is_null ? nullptr : iv)) {
    ThrowError(env, "ERR_CRYPTO_OPERATION_FAILED", "cipher initialization failed");
    return nullptr;
  }

  std::vector<uint8_t> out(in_len + std::max(32, ctx.getBlockSize() + 16));
  int out1 = 0;
  int out2 = 0;
  if (!ctx.update({input, in_len}, out.data(), &out1, false) ||
      !ctx.update({nullptr, 0}, out.data() + out1, &out2, true)) {
    ThrowError(env, "ERR_CRYPTO_OPERATION_FAILED", "cipher operation failed");
    return nullptr;
  }
  return CreateBufferCopy(env, out.data(), static_cast<size_t>(out1 + out2));
}

napi_value CryptoGetHashes(napi_env env, napi_callback_info info) {
  std::unordered_set<std::string> unique_hashes;
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
  EVP_MD_do_all_provided(
      nullptr,
      [](EVP_MD* md, void* arg) {
        if (md == nullptr || arg == nullptr) return;
        const char* name = EVP_MD_get0_name(md);
        if (name == nullptr) return;
        auto* out = static_cast<std::unordered_set<std::string>*>(arg);
        const std::string raw_name(name);
        const std::string canonical = CanonicalizeDigestName(raw_name);
        // Keep only names we can resolve through the same digest path used by
        // hashing APIs, so getHashes() round-trips with createHash()/hash().
        if (!canonical.empty() && ResolveDigest(canonical)) {
          out->emplace(canonical);
        }
      },
      &unique_hashes);
#endif
  static const char* kFallbackCandidates[] = {
      "sha1",      "sha224",    "sha256",    "sha384",    "sha512",      "shake128",   "shake256",
      "md5",       "ripemd160", "sha3-224",  "sha3-256",  "sha3-384",    "sha3-512",   "blake2b512",
      "blake2s256"};
  for (const char* candidate : kFallbackCandidates) {
    if (ResolveDigest(candidate)) unique_hashes.emplace(candidate);
  }
  if (ResolveDigest("sha1")) unique_hashes.emplace("RSA-SHA1");

  std::vector<std::string> hashes(unique_hashes.begin(), unique_hashes.end());
  std::sort(hashes.begin(), hashes.end());
  napi_value arr = nullptr;
  napi_create_array_with_length(env, hashes.size(), &arr);
  for (uint32_t i = 0; i < hashes.size(); i++) {
    napi_value s = nullptr;
    napi_create_string_utf8(env, hashes[i].c_str(), NAPI_AUTO_LENGTH, &s);
    napi_set_element(env, arr, i, s);
  }
  return arr;
}

napi_value CryptoGetCiphers(napi_env env, napi_callback_info info) {
  std::vector<std::string> names;
  ncrypto::Cipher::ForEach([&names](const char* name) { names.emplace_back(name); });
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  napi_value arr = nullptr;
  napi_create_array_with_length(env, names.size(), &arr);
  for (uint32_t i = 0; i < names.size(); i++) {
    napi_value s = nullptr;
    napi_create_string_utf8(env, names[i].c_str(), NAPI_AUTO_LENGTH, &s);
    napi_set_element(env, arr, i, s);
  }
  return arr;
}

napi_value CryptoGetSSLCiphers(napi_env env, napi_callback_info info) {
  (void)info;
  std::vector<std::string> names;
  ncrypto::SSLCtxPointer ctx = ncrypto::SSLCtxPointer::New();
  if (ctx) {
    ncrypto::SSLPointer ssl = ncrypto::SSLPointer::New(ctx);
    if (ssl) {
      ssl.getCiphers([&names](const char* name) {
        if (name != nullptr) names.emplace_back(name);
      });
    }
  }

  napi_value arr = nullptr;
  napi_create_array_with_length(env, names.size(), &arr);
  for (uint32_t i = 0; i < names.size(); i++) {
    napi_value s = nullptr;
    napi_create_string_utf8(env, names[i].c_str(), NAPI_AUTO_LENGTH, &s);
    napi_set_element(env, arr, i, s);
  }
  return arr;
}

napi_value CryptoGetCurves(napi_env env, napi_callback_info info) {
  std::vector<std::string> curves;
  ncrypto::Ec::GetCurves([&curves](const char* name) {
    if (name != nullptr) curves.emplace_back(name);
    return true;
  });
  std::sort(curves.begin(), curves.end());
  curves.erase(std::unique(curves.begin(), curves.end()), curves.end());
  napi_value arr = nullptr;
  napi_create_array_with_length(env, curves.size(), &arr);
  for (uint32_t i = 0; i < curves.size(); i++) {
    napi_value s = nullptr;
    napi_create_string_utf8(env, curves[i].c_str(), NAPI_AUTO_LENGTH, &s);
    napi_set_element(env, arr, i, s);
  }
  return arr;
}

napi_value CryptoGetCipherInfo(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) return nullptr;

  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);

  // Node native signature:
  //   getCipherInfo(infoObject, nameOrNid, keyLength?, ivLength?)
  // Keep backward compatibility for legacy single-arg call by treating argv[0]
  // as the algorithm when no info object is provided.
  bool node_signature = false;
  if (argc >= 2 && argv[0] != nullptr && argv[1] != nullptr) {
    napi_valuetype info_t = napi_undefined;
    napi_valuetype alg_t = napi_undefined;
    const bool info_ok = napi_typeof(env, argv[0], &info_t) == napi_ok;
    const bool alg_ok = napi_typeof(env, argv[1], &alg_t) == napi_ok;
    node_signature = info_ok && alg_ok && info_t == napi_object &&
                     (alg_t == napi_string || alg_t == napi_number);
  }

  napi_value out = nullptr;
  if (node_signature) {
    out = argv[0];
  } else {
    napi_create_object(env, &out);
  }
  if (out == nullptr) return undefined;

  napi_value name_or_nid = node_signature ? argv[1] : argv[0];
  napi_valuetype alg_t = napi_undefined;
  if (name_or_nid == nullptr || napi_typeof(env, name_or_nid, &alg_t) != napi_ok) {
    return undefined;
  }
  const ncrypto::Cipher cipher = [&]() -> ncrypto::Cipher {
    if (alg_t == napi_string) {
      const std::string algo = ValueToUtf8(env, name_or_nid);
      return ResolveCipher(algo);
    }
    if (alg_t == napi_number) {
      int32_t nid = 0;
      if (napi_get_value_int32(env, name_or_nid, &nid) != napi_ok) return {};
      return ncrypto::Cipher::FromNid(nid);
    }
    return {};
  }();

  if (!cipher) return undefined;

  int iv_length = cipher.getIvLength();
  int key_length = cipher.getKeyLength();
  const int block_length = cipher.getBlockSize();

  if (node_signature) {
    const bool has_key_len =
        argc >= 3 && argv[2] != nullptr && [&]() {
          napi_valuetype t = napi_undefined;
          return napi_typeof(env, argv[2], &t) == napi_ok && t == napi_number;
        }();
    const bool has_iv_len =
        argc >= 4 && argv[3] != nullptr && [&]() {
          napi_valuetype t = napi_undefined;
          return napi_typeof(env, argv[3], &t) == napi_ok && t == napi_number;
        }();

    if (has_key_len || has_iv_len) {
      ncrypto::CipherCtxPointer ctx = ncrypto::CipherCtxPointer::New();
      if (!ctx || !ctx.init(cipher, true)) return undefined;

      if (has_key_len) {
        int32_t requested_key_len = 0;
        if (napi_get_value_int32(env, argv[2], &requested_key_len) != napi_ok) return undefined;
        if (!ctx.setKeyLength(static_cast<size_t>(requested_key_len))) return undefined;
        key_length = requested_key_len;
      }

      if (has_iv_len) {
        int32_t requested_iv_len = 0;
        if (napi_get_value_int32(env, argv[3], &requested_iv_len) != napi_ok) return undefined;
        if (cipher.isCcmMode()) {
          if (requested_iv_len < 7 || requested_iv_len > 13) return undefined;
        } else if (cipher.isGcmMode()) {
          // Node accepts any iv length for GCM here and defers validation.
        } else if (cipher.isOcbMode()) {
          if (!ctx.setIvLength(static_cast<size_t>(requested_iv_len))) return undefined;
        } else if (requested_iv_len != iv_length) {
          return undefined;
        }
        iv_length = requested_iv_len;
      }
    }
  }

  const std::string_view mode = cipher.getModeLabel();
  if (!mode.empty()) {
    napi_value mode_v = nullptr;
    napi_create_string_utf8(env, mode.data(), mode.size(), &mode_v);
    if (mode_v != nullptr) napi_set_named_property(env, out, "mode", mode_v);
  }

  if (const char* cipher_name = cipher.getName(); cipher_name != nullptr && cipher_name[0] != '\0') {
    napi_value name_v = nullptr;
    napi_create_string_utf8(env, cipher_name, NAPI_AUTO_LENGTH, &name_v);
    if (name_v != nullptr) napi_set_named_property(env, out, "name", name_v);
  }

  napi_value nid_v = nullptr;
  napi_create_int32(env, cipher.getNid(), &nid_v);
  if (nid_v != nullptr) napi_set_named_property(env, out, "nid", nid_v);

  if (!cipher.isStreamMode()) {
    napi_value block_v = nullptr;
    napi_create_int32(env, block_length, &block_v);
    if (block_v != nullptr) napi_set_named_property(env, out, "blockSize", block_v);
  }

  if (iv_length != 0) {
    napi_value iv_v = nullptr;
    napi_create_int32(env, iv_length, &iv_v);
    if (iv_v != nullptr) napi_set_named_property(env, out, "ivLength", iv_v);
  }

  napi_value key_v = nullptr;
  napi_create_int32(env, key_length, &key_v);
  if (key_v != nullptr) napi_set_named_property(env, out, "keyLength", key_v);

  return out;
}

napi_value CryptoParsePfx(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) return nullptr;
  uint8_t* pfx = nullptr;
  size_t pfx_len = 0;
  if (!GetBufferBytes(env, argv[0], &pfx, &pfx_len)) return nullptr;
  std::string pass;
  bool has_pass = false;
  if (argc >= 2 && !ReadPassphrase(env, argv[1], &pass, &has_pass)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "passphrase must be a string or Buffer");
    return nullptr;
  }
  const unsigned char* p = pfx;
  PKCS12* pkcs12 = d2i_PKCS12(nullptr, &p, static_cast<long>(pfx_len));
  if (pkcs12 == nullptr) {
    ThrowError(env, "ERR_CRYPTO_PFX", "not enough data");
    return nullptr;
  }
  EVP_PKEY* pkey = nullptr;
  X509* cert = nullptr;
  STACK_OF(X509)* ca = nullptr;
  const int ok = PKCS12_parse(pkcs12, has_pass ? pass.c_str() : nullptr, &pkey, &cert, &ca);
  PKCS12_free(pkcs12);
  if (pkey) EVP_PKEY_free(pkey);
  if (cert) X509_free(cert);
  if (ca) sk_X509_pop_free(ca, X509_free);
  if (ok != 1) {
    ThrowError(env, "ERR_CRYPTO_PFX", "mac verify failure");
    return nullptr;
  }
  napi_value true_v = nullptr;
  napi_get_boolean(env, true, &true_v);
  return true_v;
}

napi_value CryptoParseCrl(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) return nullptr;
  uint8_t* crl_data = nullptr;
  size_t crl_len = 0;
  if (!GetBufferBytes(env, argv[0], &crl_data, &crl_len)) return nullptr;
  BIO* bio = BIO_new_mem_buf(crl_data, static_cast<int>(crl_len));
  X509_CRL* crl = bio ? PEM_read_bio_X509_CRL(bio, nullptr, nullptr, nullptr) : nullptr;
  if (crl == nullptr && bio != nullptr) {
    (void)BIO_reset(bio);
    crl = d2i_X509_CRL_bio(bio, nullptr);
  }
  if (crl) X509_CRL_free(crl);
  if (bio) BIO_free(bio);
  if (crl == nullptr) {
    ThrowError(env, "ERR_CRYPTO_CRL", "Failed to parse CRL");
    return nullptr;
  }
  napi_value true_v = nullptr;
  napi_get_boolean(env, true, &true_v);
  return true_v;
}

napi_value CryptoSecureContextCreate(napi_env env, napi_callback_info info) {
  (void)info;
  SSL_CTX* ctx = SSL_CTX_new(TLS_method());
  if (ctx == nullptr) {
    ThrowLastOpenSslError(env, "ERR_TLS_INVALID_CONTEXT", "Failed to create secure context");
    return nullptr;
  }
  auto* holder = new SecureContextHolder(ctx);
  napi_value out = nullptr;
  if (napi_create_external(env, holder, SecureContextFinalizer, nullptr, &out) != napi_ok || out == nullptr) {
    delete holder;
    return nullptr;
  }
  return out;
}

napi_value CryptoSecureContextInit(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) return nullptr;
  SecureContextHolder* holder = nullptr;
  if (!GetSecureContextHolder(env, argv[0], &holder) || holder == nullptr || holder->ctx == nullptr) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "context must be a secure context handle");
    return nullptr;
  }
  int32_t min_version = 0;
  int32_t max_version = 0;
  if (argc >= 2 && argv[1] != nullptr) napi_get_value_int32(env, argv[1], &min_version);
  if (argc >= 3 && argv[2] != nullptr) napi_get_value_int32(env, argv[2], &max_version);
  if (min_version > 0 && SSL_CTX_set_min_proto_version(holder->ctx, min_version) != 1) {
    ThrowLastOpenSslError(env, "ERR_TLS_INVALID_PROTOCOL_VERSION", "Failed to set min protocol version");
    return nullptr;
  }
  if (max_version > 0 && SSL_CTX_set_max_proto_version(holder->ctx, max_version) != 1) {
    ThrowLastOpenSslError(env, "ERR_TLS_INVALID_PROTOCOL_VERSION", "Failed to set max protocol version");
    return nullptr;
  }
  napi_value true_v = nullptr;
  napi_get_boolean(env, true, &true_v);
  return true_v;
}

napi_value CryptoSecureContextSetOptions(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 2) return nullptr;
  SecureContextHolder* holder = nullptr;
  if (!GetSecureContextHolder(env, argv[0], &holder) || holder == nullptr || holder->ctx == nullptr) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "context must be a secure context handle");
    return nullptr;
  }
  int64_t options = 0;
  if (napi_get_value_int64(env, argv[1], &options) != napi_ok) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "secure options must be an integer");
    return nullptr;
  }
  SSL_CTX_set_options(holder->ctx, static_cast<uint64_t>(options));
  napi_value true_v = nullptr;
  napi_get_boolean(env, true, &true_v);
  return true_v;
}

napi_value CryptoSecureContextSetCiphers(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 2) return nullptr;
  SecureContextHolder* holder = nullptr;
  if (!GetSecureContextHolder(env, argv[0], &holder) || holder == nullptr || holder->ctx == nullptr) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "context must be a secure context handle");
    return nullptr;
  }
  const std::string ciphers = ValueToUtf8(env, argv[1]);
  if (SSL_CTX_set_cipher_list(holder->ctx, ciphers.c_str()) != 1) {
    ThrowLastOpenSslError(env, "ERR_TLS_INVALID_CIPHER", "Failed to set ciphers");
    return nullptr;
  }
  napi_value true_v = nullptr;
  napi_get_boolean(env, true, &true_v);
  return true_v;
}

napi_value CryptoSecureContextSetCipherSuites(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 2) return nullptr;
  SecureContextHolder* holder = nullptr;
  if (!GetSecureContextHolder(env, argv[0], &holder) || holder == nullptr || holder->ctx == nullptr) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "context must be a secure context handle");
    return nullptr;
  }
  const std::string suites = ValueToUtf8(env, argv[1]);
#if OPENSSL_VERSION_NUMBER >= 0x10101000L
  if (SSL_CTX_set_ciphersuites(holder->ctx, suites.c_str()) != 1) {
    ThrowLastOpenSslError(env, "ERR_TLS_INVALID_CIPHER", "Failed to set TLSv1.3 ciphersuites");
    return nullptr;
  }
#else
  (void)suites;
#endif
  napi_value true_v = nullptr;
  napi_get_boolean(env, true, &true_v);
  return true_v;
}

napi_value CryptoSecureContextSetCert(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 2) return nullptr;
  SecureContextHolder* holder = nullptr;
  if (!GetSecureContextHolder(env, argv[0], &holder) || holder == nullptr || holder->ctx == nullptr) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "context must be a secure context handle");
    return nullptr;
  }
  std::vector<uint8_t> cert_owned;
  uint8_t* cert_bytes = nullptr;
  size_t cert_len = 0;
  if (!GetBufferOrStringBytes(env, argv[1], &cert_owned, &cert_bytes, &cert_len)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "cert must be a string or Buffer");
    return nullptr;
  }
  X509* cert = ParseX509(cert_bytes, cert_len);
  if (cert == nullptr) {
    ThrowLastOpenSslError(env, "ERR_OSSL_PEM_NO_START_LINE", "Failed to parse certificate");
    return nullptr;
  }
  const int ok = SSL_CTX_use_certificate(holder->ctx, cert);
  X509_free(cert);
  if (ok != 1) {
    ThrowLastOpenSslError(env, "ERR_TLS_CERT", "Failed to use certificate");
    return nullptr;
  }
  napi_value true_v = nullptr;
  napi_get_boolean(env, true, &true_v);
  return true_v;
}

napi_value CryptoSecureContextSetKey(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 2) return nullptr;
  SecureContextHolder* holder = nullptr;
  if (!GetSecureContextHolder(env, argv[0], &holder) || holder == nullptr || holder->ctx == nullptr) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "context must be a secure context handle");
    return nullptr;
  }
  std::vector<uint8_t> key_owned;
  uint8_t* key_bytes = nullptr;
  size_t key_len = 0;
  if (!GetBufferOrStringBytes(env, argv[1], &key_owned, &key_bytes, &key_len)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "key must be a string or Buffer");
    return nullptr;
  }
  std::string passphrase;
  bool has_passphrase = false;
  if (argc >= 3 && !ReadPassphrase(env, argv[2], &passphrase, &has_passphrase)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "passphrase must be a string or Buffer");
    return nullptr;
  }
  BIO* bio = BIO_new_mem_buf(key_bytes, static_cast<int>(key_len));
  if (bio == nullptr) return nullptr;
  EVP_PKEY* pkey = PEM_read_bio_PrivateKey(
      bio,
      nullptr,
      nullptr,
      has_passphrase ? const_cast<char*>(passphrase.c_str()) : nullptr);
  if (pkey == nullptr) {
    (void)BIO_reset(bio);
    pkey = d2i_PrivateKey_bio(bio, nullptr);
  }
  BIO_free(bio);
  if (pkey == nullptr) {
    ThrowLastOpenSslError(env, "ERR_OSSL_PEM_NO_START_LINE", "Failed to parse private key");
    return nullptr;
  }
  const int ok = SSL_CTX_use_PrivateKey(holder->ctx, pkey);
  EVP_PKEY_free(pkey);
  if (ok != 1) {
    ThrowLastOpenSslError(env, "ERR_TLS_KEY", "Failed to use private key");
    return nullptr;
  }
  napi_value true_v = nullptr;
  napi_get_boolean(env, true, &true_v);
  return true_v;
}

napi_value CryptoSecureContextAddCACert(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 2) return nullptr;
  SecureContextHolder* holder = nullptr;
  if (!GetSecureContextHolder(env, argv[0], &holder) || holder == nullptr || holder->ctx == nullptr) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "context must be a secure context handle");
    return nullptr;
  }
  std::vector<uint8_t> cert_owned;
  uint8_t* cert_bytes = nullptr;
  size_t cert_len = 0;
  if (!GetBufferOrStringBytes(env, argv[1], &cert_owned, &cert_bytes, &cert_len)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "ca must be a string or Buffer");
    return nullptr;
  }
  X509* cert = ParseX509(cert_bytes, cert_len);
  if (cert == nullptr) {
    ThrowLastOpenSslError(env, "ERR_OSSL_PEM_NO_START_LINE", "Failed to parse CA certificate");
    return nullptr;
  }
  X509_STORE* store = SSL_CTX_get_cert_store(holder->ctx);
  if (store == nullptr || X509_STORE_add_cert(store, cert) != 1) {
    X509_free(cert);
    ThrowLastOpenSslError(env, "ERR_TLS_CERT", "Failed to add CA certificate");
    return nullptr;
  }
  X509_free(cert);
  napi_value true_v = nullptr;
  napi_get_boolean(env, true, &true_v);
  return true_v;
}

napi_value CryptoSecureContextAddCrl(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 2) return nullptr;
  SecureContextHolder* holder = nullptr;
  if (!GetSecureContextHolder(env, argv[0], &holder) || holder == nullptr || holder->ctx == nullptr) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "context must be a secure context handle");
    return nullptr;
  }
  std::vector<uint8_t> crl_owned;
  uint8_t* crl_bytes = nullptr;
  size_t crl_len = 0;
  if (!GetBufferOrStringBytes(env, argv[1], &crl_owned, &crl_bytes, &crl_len)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "crl must be a string or Buffer");
    return nullptr;
  }
  X509_CRL* crl = ParseX509Crl(crl_bytes, crl_len);
  if (crl == nullptr) {
    ThrowError(env, "ERR_CRYPTO_CRL", "Failed to parse CRL");
    return nullptr;
  }
  X509_STORE* store = SSL_CTX_get_cert_store(holder->ctx);
  if (store == nullptr || X509_STORE_add_crl(store, crl) != 1) {
    X509_CRL_free(crl);
    ThrowLastOpenSslError(env, "ERR_TLS_CERT", "Failed to add CRL");
    return nullptr;
  }
  X509_CRL_free(crl);
  X509_STORE_set_flags(store, X509_V_FLAG_CRL_CHECK | X509_V_FLAG_CRL_CHECK_ALL);
  napi_value true_v = nullptr;
  napi_get_boolean(env, true, &true_v);
  return true_v;
}

EVP_PKEY* ParsePrivateKeyWithPassphrase(const uint8_t* data,
                                        size_t len,
                                        const std::string& passphrase,
                                        bool has_passphrase) {
  struct PasswordCallbackData {
    const unsigned char* data = nullptr;
    size_t len = 0;
    bool provided = false;
  };
  auto password_callback = [](char* buf, int size, int /*rwflag*/, void* userdata) -> int {
    auto* cb_data = static_cast<PasswordCallbackData*>(userdata);
    if (cb_data == nullptr || !cb_data->provided) return -1;
    if (size <= 0) return 0;
    const size_t max_copy = static_cast<size_t>(size);
    const size_t copy_len = cb_data->len < max_copy ? cb_data->len : max_copy;
    if (copy_len > 0 && cb_data->data != nullptr) {
      std::memcpy(buf, cb_data->data, copy_len);
    }
    return static_cast<int>(copy_len);
  };

  BIO* bio = BIO_new_mem_buf(data, static_cast<int>(len));
  if (bio == nullptr) return nullptr;
  PasswordCallbackData cb_data;
  cb_data.data = has_passphrase ? reinterpret_cast<const unsigned char*>(passphrase.data()) : nullptr;
  cb_data.len = has_passphrase ? passphrase.size() : 0;
  cb_data.provided = has_passphrase;
  void* passphrase_arg = &cb_data;
  pem_password_cb* password_cb = password_callback;
  EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, password_cb, passphrase_arg);
  const bool looks_like_pem = (len > 0 && data[0] == '-');
  if (pkey == nullptr) {
    (void)BIO_reset(bio);
    pkey = d2i_PKCS8PrivateKey_bio(bio, nullptr, password_cb, passphrase_arg);
  }
  if (pkey == nullptr && !looks_like_pem) {
    (void)BIO_reset(bio);
    pkey = d2i_PrivateKey_bio(bio, nullptr);
  }
  BIO_free(bio);
  if (pkey != nullptr) ERR_clear_error();
  return pkey;
}

EVP_PKEY* ParsePrivateKey(const uint8_t* data, size_t len) {
  return ParsePrivateKeyWithPassphrase(data, len, "", false);
}

EVP_PKEY* ParsePublicKeyOrCert(const uint8_t* data, size_t len) {
  BIO* bio = BIO_new_mem_buf(data, static_cast<int>(len));
  if (bio == nullptr) return nullptr;
  const bool looks_like_pem = (len > 0 && data[0] == '-');
  EVP_PKEY* pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
  if (pkey == nullptr) {
    (void)BIO_reset(bio);
    RSA* rsa = PEM_read_bio_RSAPublicKey(bio, nullptr, nullptr, nullptr);
    if (rsa != nullptr) {
      pkey = EVP_PKEY_new();
      if (pkey == nullptr || EVP_PKEY_assign_RSA(pkey, rsa) != 1) {
        if (pkey != nullptr) EVP_PKEY_free(pkey);
        RSA_free(rsa);
        pkey = nullptr;
      }
    }
  }
  if (pkey == nullptr && !looks_like_pem) {
    (void)BIO_reset(bio);
    pkey = d2i_PUBKEY_bio(bio, nullptr);
  }
  if (pkey == nullptr && !looks_like_pem) {
    (void)BIO_reset(bio);
    RSA* rsa = d2i_RSAPublicKey_bio(bio, nullptr);
    if (rsa != nullptr) {
      pkey = EVP_PKEY_new();
      if (pkey == nullptr || EVP_PKEY_assign_RSA(pkey, rsa) != 1) {
        if (pkey != nullptr) EVP_PKEY_free(pkey);
        RSA_free(rsa);
        pkey = nullptr;
      }
    }
  }
  if (pkey == nullptr) {
    (void)BIO_reset(bio);
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    if (cert != nullptr) {
      pkey = X509_get_pubkey(cert);
      X509_free(cert);
    }
  }
  BIO_free(bio);
  if (pkey != nullptr) ERR_clear_error();
  return pkey;
}

X509* ParseX509(const uint8_t* data, size_t len) {
  BIO* bio = BIO_new_mem_buf(data, static_cast<int>(len));
  if (bio == nullptr) return nullptr;
  X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
  if (cert == nullptr) {
    (void)BIO_reset(bio);
    cert = d2i_X509_bio(bio, nullptr);
  }
  BIO_free(bio);
  return cert;
}

X509_CRL* ParseX509Crl(const uint8_t* data, size_t len) {
  BIO* bio = BIO_new_mem_buf(data, static_cast<int>(len));
  if (bio == nullptr) return nullptr;
  X509_CRL* crl = PEM_read_bio_X509_CRL(bio, nullptr, nullptr, nullptr);
  if (crl == nullptr) {
    (void)BIO_reset(bio);
    crl = d2i_X509_CRL_bio(bio, nullptr);
  }
  BIO_free(bio);
  return crl;
}

std::string AsymmetricKeyTypeName(const EVP_PKEY* pkey) {
  if (pkey == nullptr) return "";
  switch (EVP_PKEY_base_id(pkey)) {
    case EVP_PKEY_RSA:
      return "rsa";
    case EVP_PKEY_RSA_PSS:
      return "rsa-pss";
    case EVP_PKEY_DSA:
      return "dsa";
    case EVP_PKEY_DH:
      return "dh";
    case EVP_PKEY_EC:
      return "ec";
    case EVP_PKEY_ED25519:
      return "ed25519";
    case EVP_PKEY_ED448:
      return "ed448";
    case EVP_PKEY_X25519:
      return "x25519";
    case EVP_PKEY_X448:
      return "x448";
    default:
      return "";
  }
}

size_t GetDsaSigPartLengthFromPkey(EVP_PKEY* pkey) {
  if (pkey == nullptr) return 0;
  const int type = EVP_PKEY_base_id(pkey);
  if (type == EVP_PKEY_EC) {
    EC_KEY* ec = EVP_PKEY_get1_EC_KEY(pkey);
    if (ec == nullptr) return 0;
    const EC_GROUP* group = EC_KEY_get0_group(ec);
    const int bits = group != nullptr ? EC_GROUP_order_bits(group) : 0;
    EC_KEY_free(ec);
    if (bits <= 0) return 0;
    return static_cast<size_t>((bits + 7) / 8);
  }
  if (type == EVP_PKEY_DSA) {
    DSA* dsa = EVP_PKEY_get1_DSA(pkey);
    if (dsa == nullptr) return 0;
    const BIGNUM* p = nullptr;
    const BIGNUM* q = nullptr;
    const BIGNUM* g = nullptr;
    DSA_get0_pqg(dsa, &p, &q, &g);
    const int bits = q != nullptr ? BN_num_bits(q) : 0;
    DSA_free(dsa);
    if (bits <= 0) return 0;
    return static_cast<size_t>((bits + 7) / 8);
  }
  return 0;
}

bool DerSignatureToP1363(const uint8_t* der,
                         size_t der_len,
                         size_t part_len,
                         std::vector<uint8_t>* out) {
  if (der == nullptr || out == nullptr || part_len == 0) return false;
  out->assign(part_len * 2, 0);
  ncrypto::Buffer<const unsigned char> der_buf{der, der_len};
  return ncrypto::extractP1363(der_buf, out->data(), part_len);
}

bool P1363ToDerSignature(int pkey_type,
                         const uint8_t* sig,
                         size_t sig_len,
                         size_t part_len,
                         std::vector<uint8_t>* out) {
  if (sig == nullptr || out == nullptr || part_len == 0 || sig_len != part_len * 2) return false;
  BIGNUM* r = BN_bin2bn(sig, static_cast<int>(part_len), nullptr);
  BIGNUM* s = BN_bin2bn(sig + part_len, static_cast<int>(part_len), nullptr);
  if (r == nullptr || s == nullptr) {
    if (r != nullptr) BN_free(r);
    if (s != nullptr) BN_free(s);
    return false;
  }

  unsigned char* der = nullptr;
  int der_len = 0;
  if (pkey_type == EVP_PKEY_EC) {
    ECDSA_SIG* ec_sig = ECDSA_SIG_new();
    if (ec_sig == nullptr || ECDSA_SIG_set0(ec_sig, r, s) != 1) {
      if (ec_sig != nullptr) ECDSA_SIG_free(ec_sig);
      BN_free(r);
      BN_free(s);
      return false;
    }
    der_len = i2d_ECDSA_SIG(ec_sig, &der);
    ECDSA_SIG_free(ec_sig);
  } else if (pkey_type == EVP_PKEY_DSA) {
    DSA_SIG* dsa_sig = DSA_SIG_new();
    if (dsa_sig == nullptr || DSA_SIG_set0(dsa_sig, r, s) != 1) {
      if (dsa_sig != nullptr) DSA_SIG_free(dsa_sig);
      BN_free(r);
      BN_free(s);
      return false;
    }
    der_len = i2d_DSA_SIG(dsa_sig, &der);
    DSA_SIG_free(dsa_sig);
  } else {
    BN_free(r);
    BN_free(s);
    return false;
  }

  if (der == nullptr || der_len <= 0) {
    if (der != nullptr) OPENSSL_free(der);
    return false;
  }
  out->assign(der, der + der_len);
  OPENSSL_free(der);
  return true;
}

napi_value CryptoGetAsymmetricKeyDetails(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) return nullptr;

  uint8_t* key_bytes = nullptr;
  size_t key_len = 0;
  if (!GetBufferBytes(env, argv[0], &key_bytes, &key_len)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "key must be a Buffer");
    return nullptr;
  }
  std::string passphrase;
  bool has_passphrase = false;
  if (argc >= 2 && argv[1] != nullptr && !IsNullOrUndefined(env, argv[1])) {
    if (!ReadPassphrase(env, argv[1], &passphrase, &has_passphrase)) {
      ThrowError(env, "ERR_INVALID_ARG_TYPE", "Invalid key passphrase");
      return nullptr;
    }
  }

  EVP_PKEY* pkey = ParsePrivateKeyWithPassphrase(key_bytes, key_len, passphrase, has_passphrase);
  if (pkey == nullptr) pkey = ParsePublicKeyOrCert(key_bytes, key_len);
  if (pkey == nullptr) {
    napi_value null_v = nullptr;
    napi_get_null(env, &null_v);
    return null_v;
  }

  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) {
    EVP_PKEY_free(pkey);
    return nullptr;
  }

  auto set_int32 = [&](const char* name, int32_t value) {
    napi_value v = nullptr;
    if (napi_create_int32(env, value, &v) == napi_ok && v != nullptr) {
      napi_set_named_property(env, out, name, v);
    }
  };
  auto set_string = [&](const char* name, std::string value) {
    if (value.empty()) return;
    napi_value v = nullptr;
    if (napi_create_string_utf8(env, value.c_str(), NAPI_AUTO_LENGTH, &v) == napi_ok && v != nullptr) {
      napi_set_named_property(env, out, name, v);
    }
  };
  auto set_buffer = [&](const char* name, const std::vector<uint8_t>& bytes) {
    napi_value v = CreateBufferCopy(env, bytes.data(), bytes.size());
    if (v != nullptr) napi_set_named_property(env, out, name, v);
  };
  auto normalize_digest_name = [](std::string in) {
    for (char& ch : in) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (in.rfind("sha2-", 0) == 0) in.erase(3, 2);  // sha2-256 -> sha256
    return in;
  };

  const int pkey_type = EVP_PKEY_base_id(pkey);
  if (pkey_type == EVP_PKEY_RSA || pkey_type == EVP_PKEY_RSA_PSS) {
    const int bits = EVP_PKEY_bits(pkey);
    if (bits > 0) set_int32("modulusLength", bits);

    BIGNUM* e = nullptr;
    if (EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_E, &e) == 1 && e != nullptr) {
      const int e_len = BN_num_bytes(e);
      if (e_len > 0) {
        std::vector<uint8_t> e_bytes(static_cast<size_t>(e_len));
        BN_bn2bin(e, e_bytes.data());
        set_buffer("publicExponent", e_bytes);
      }
      BN_free(e);
    }

    if (pkey_type == EVP_PKEY_RSA_PSS) {
      char hash_name[64];
      size_t hash_name_len = 0;
      if (EVP_PKEY_get_utf8_string_param(
              pkey, OSSL_PKEY_PARAM_RSA_DIGEST, hash_name, sizeof(hash_name), &hash_name_len) == 1 &&
          hash_name_len > 0) {
        set_string("hashAlgorithm", normalize_digest_name(std::string(hash_name, hash_name_len)));
      }

      char mgf1_name[64];
      size_t mgf1_name_len = 0;
      if (EVP_PKEY_get_utf8_string_param(
              pkey, OSSL_PKEY_PARAM_RSA_MGF1_DIGEST, mgf1_name, sizeof(mgf1_name), &mgf1_name_len) == 1 &&
          mgf1_name_len > 0) {
        set_string("mgf1HashAlgorithm", normalize_digest_name(std::string(mgf1_name, mgf1_name_len)));
      }

      int salt_len = -1;
      if (EVP_PKEY_get_int_param(pkey, OSSL_PKEY_PARAM_RSA_PSS_SALTLEN, &salt_len) == 1 && salt_len >= 0) {
        set_int32("saltLength", salt_len);
      }
    }
  } else if (pkey_type == EVP_PKEY_DSA) {
    const int bits = EVP_PKEY_bits(pkey);
    if (bits > 0) set_int32("modulusLength", bits);

    int q_bits = 0;
    if (EVP_PKEY_get_int_param(pkey, OSSL_PKEY_PARAM_FFC_QBITS, &q_bits) == 1 && q_bits > 0) {
      set_int32("divisorLength", q_bits);
    } else {
      BIGNUM* q = nullptr;
      if (EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_FFC_Q, &q) == 1 && q != nullptr) {
        const int q_len = BN_num_bits(q);
        if (q_len > 0) set_int32("divisorLength", q_len);
        BN_free(q);
      }
    }
  } else if (pkey_type == EVP_PKEY_EC) {
    char group_name[80];
    size_t group_name_len = 0;
    if (EVP_PKEY_get_utf8_string_param(
            pkey, OSSL_PKEY_PARAM_GROUP_NAME, group_name, sizeof(group_name), &group_name_len) == 1 &&
        group_name_len > 0) {
      set_string("namedCurve", std::string(group_name, group_name_len));
    }
  }

  EVP_PKEY_free(pkey);
  return out;
}

napi_value CryptoGetAsymmetricKeyType(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) return nullptr;
  uint8_t* key_bytes = nullptr;
  size_t key_len = 0;
  if (!GetBufferBytes(env, argv[0], &key_bytes, &key_len)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "key must be a Buffer");
    return nullptr;
  }
  std::string passphrase;
  bool has_passphrase = false;
  if (argc >= 2 && argv[1] != nullptr && !IsNullOrUndefined(env, argv[1])) {
    if (!ReadPassphrase(env, argv[1], &passphrase, &has_passphrase)) {
      ThrowError(env, "ERR_INVALID_ARG_TYPE", "Invalid key passphrase");
      return nullptr;
    }
  }
  EVP_PKEY* pkey = ParsePrivateKeyWithPassphrase(key_bytes, key_len, passphrase, has_passphrase);
  if (pkey == nullptr) pkey = ParsePublicKeyOrCert(key_bytes, key_len);
  if (pkey == nullptr) {
    napi_value null_v = nullptr;
    napi_get_null(env, &null_v);
    return null_v;
  }
  const std::string type = AsymmetricKeyTypeName(pkey);
  EVP_PKEY_free(pkey);
  if (type.empty()) {
    napi_value null_v = nullptr;
    napi_get_null(env, &null_v);
    return null_v;
  }
  napi_value out = nullptr;
  napi_create_string_utf8(env, type.c_str(), NAPI_AUTO_LENGTH, &out);
  return out;
}

napi_value CryptoPublicEncrypt(napi_env env, napi_callback_info info) {
  size_t argc = 8;
  napi_value argv[8] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 5) return nullptr;
  std::vector<uint8_t> owned_key;
  uint8_t* key_bytes = nullptr;
  size_t key_len = 0;
  uint8_t* input = nullptr;
  size_t in_len = 0;
  if (!GetKeyBytes(env, argv[0], &owned_key, &key_bytes, &key_len) ||
      !GetBufferBytes(env, argv[4], &input, &in_len)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "key and buffer must be Buffers or strings");
    return nullptr;
  }
  int32_t padding = RSA_PKCS1_OAEP_PADDING;
  if (argc >= 6 && argv[5] != nullptr) napi_get_value_int32(env, argv[5], &padding);
  std::string oaep_hash = "sha1";
  napi_valuetype hash_type = napi_undefined;
  if (argc >= 7 && argv[6] != nullptr &&
      napi_typeof(env, argv[6], &hash_type) == napi_ok &&
      hash_type == napi_string) {
    oaep_hash = ValueToUtf8(env, argv[6]);
  }
  uint8_t* label = nullptr;
  size_t label_len = 0;
  bool has_label = (argc >= 8 && argv[7] != nullptr && GetBufferBytes(env, argv[7], &label, &label_len));
  std::string passphrase;
  bool has_passphrase = false;
  if (argc >= 4 && argv[3] != nullptr && !ReadPassphrase(env, argv[3], &passphrase, &has_passphrase)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "Invalid key passphrase");
    return nullptr;
  }

  EVP_PKEY* pkey = ParsePublicKeyOrCert(key_bytes, key_len);
  if (pkey == nullptr) pkey = ParsePrivateKeyWithPassphrase(key_bytes, key_len, passphrase, has_passphrase);
  if (pkey == nullptr) {
    ThrowLastOpenSslError(env, "ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE", "Invalid public key");
    return nullptr;
  }
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(pkey, nullptr);
  if (ctx == nullptr || EVP_PKEY_encrypt_init(ctx) != 1 ||
      EVP_PKEY_CTX_set_rsa_padding(ctx, padding) != 1) {
    if (ctx) EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    ThrowLastOpenSslError(env, "ERR_CRYPTO_OPERATION_FAILED", "publicEncrypt initialization failed");
    return nullptr;
  }
  if (padding == RSA_PKCS1_OAEP_PADDING) {
    const EVP_MD* md = EVP_get_digestbyname(oaep_hash.c_str());
    if (md == nullptr || EVP_PKEY_CTX_set_rsa_oaep_md(ctx, md) != 1 ||
        EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, md) != 1) {
      EVP_PKEY_CTX_free(ctx);
      EVP_PKEY_free(pkey);
      ThrowLastOpenSslError(env, "ERR_CRYPTO_INVALID_DIGEST", "Invalid OAEP digest");
      return nullptr;
    }
    if (has_label && label_len > 0) {
      unsigned char* copied = reinterpret_cast<unsigned char*>(OPENSSL_malloc(label_len));
      if (copied == nullptr) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        ThrowError(env, "ERR_CRYPTO_OPERATION_FAILED", "Failed to allocate OAEP label");
        return nullptr;
      }
      std::memcpy(copied, label, label_len);
      if (EVP_PKEY_CTX_set0_rsa_oaep_label(ctx, copied, static_cast<int>(label_len)) != 1) {
        OPENSSL_free(copied);
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        ThrowLastOpenSslError(env, "ERR_CRYPTO_OPERATION_FAILED", "Failed to set OAEP label");
        return nullptr;
      }
    }
  }
  size_t out_len = 0;
  if (EVP_PKEY_encrypt(ctx, nullptr, &out_len, input, in_len) != 1) {
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    ThrowLastOpenSslError(env, "ERR_CRYPTO_OPERATION_FAILED", "publicEncrypt failed");
    return nullptr;
  }
  std::vector<uint8_t> out(out_len);
  if (EVP_PKEY_encrypt(ctx, out.data(), &out_len, input, in_len) != 1) {
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    ThrowLastOpenSslError(env, "ERR_CRYPTO_OPERATION_FAILED", "publicEncrypt failed");
    return nullptr;
  }
  EVP_PKEY_CTX_free(ctx);
  EVP_PKEY_free(pkey);
  return CreateBufferCopy(env, out.data(), out_len);
}

napi_value CryptoPrivateDecrypt(napi_env env, napi_callback_info info) {
  size_t argc = 8;
  napi_value argv[8] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 5) return nullptr;
  std::vector<uint8_t> owned_key;
  uint8_t* key_bytes = nullptr;
  size_t key_len = 0;
  uint8_t* input = nullptr;
  size_t in_len = 0;
  if (!GetKeyBytes(env, argv[0], &owned_key, &key_bytes, &key_len) ||
      !GetBufferBytes(env, argv[4], &input, &in_len)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "key and buffer must be Buffers or strings");
    return nullptr;
  }
  int32_t padding = RSA_PKCS1_OAEP_PADDING;
  if (argc >= 6 && argv[5] != nullptr) napi_get_value_int32(env, argv[5], &padding);
  std::string oaep_hash = "sha1";
  napi_valuetype hash_type = napi_undefined;
  if (argc >= 7 && argv[6] != nullptr &&
      napi_typeof(env, argv[6], &hash_type) == napi_ok &&
      hash_type == napi_string) {
    oaep_hash = ValueToUtf8(env, argv[6]);
  }
  uint8_t* label = nullptr;
  size_t label_len = 0;
  bool has_label = (argc >= 8 && argv[7] != nullptr && GetBufferBytes(env, argv[7], &label, &label_len));

  std::string passphrase;
  bool has_passphrase = false;
  if (argc >= 4 && argv[3] != nullptr && !ReadPassphrase(env, argv[3], &passphrase, &has_passphrase)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "Invalid key passphrase");
    return nullptr;
  }

  EVP_PKEY* pkey = ParsePrivateKeyWithPassphrase(key_bytes, key_len, passphrase, has_passphrase);
  if (pkey == nullptr) {
    ThrowLastOpenSslError(env, "ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE", "Invalid private key");
    return nullptr;
  }
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(pkey, nullptr);
  if (ctx == nullptr || EVP_PKEY_decrypt_init(ctx) != 1 ||
      EVP_PKEY_CTX_set_rsa_padding(ctx, padding) != 1) {
    if (ctx) EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    ThrowLastOpenSslError(env, "ERR_CRYPTO_OPERATION_FAILED", "privateDecrypt initialization failed");
    return nullptr;
  }
  if (padding == RSA_PKCS1_OAEP_PADDING) {
    const EVP_MD* md = EVP_get_digestbyname(oaep_hash.c_str());
    if (md == nullptr || EVP_PKEY_CTX_set_rsa_oaep_md(ctx, md) != 1 ||
        EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, md) != 1) {
      EVP_PKEY_CTX_free(ctx);
      EVP_PKEY_free(pkey);
      ThrowLastOpenSslError(env, "ERR_CRYPTO_INVALID_DIGEST", "Invalid OAEP digest");
      return nullptr;
    }
    if (has_label && label_len > 0) {
      unsigned char* copied = reinterpret_cast<unsigned char*>(OPENSSL_malloc(label_len));
      if (copied == nullptr) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        ThrowError(env, "ERR_CRYPTO_OPERATION_FAILED", "Failed to allocate OAEP label");
        return nullptr;
      }
      std::memcpy(copied, label, label_len);
      if (EVP_PKEY_CTX_set0_rsa_oaep_label(ctx, copied, static_cast<int>(label_len)) != 1) {
        OPENSSL_free(copied);
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        ThrowLastOpenSslError(env, "ERR_CRYPTO_OPERATION_FAILED", "Failed to set OAEP label");
        return nullptr;
      }
    }
  }
  size_t out_len = 0;
  if (EVP_PKEY_decrypt(ctx, nullptr, &out_len, input, in_len) != 1) {
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    ThrowLastOpenSslError(env, "ERR_CRYPTO_OPERATION_FAILED", "privateDecrypt failed");
    return nullptr;
  }
  std::vector<uint8_t> out(out_len);
  if (EVP_PKEY_decrypt(ctx, out.data(), &out_len, input, in_len) != 1) {
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    ThrowLastOpenSslError(env, "ERR_CRYPTO_OPERATION_FAILED", "privateDecrypt failed");
    return nullptr;
  }
  EVP_PKEY_CTX_free(ctx);
  EVP_PKEY_free(pkey);
  return CreateBufferCopy(env, out.data(), out_len);
}

napi_value CryptoCipherTransformAead(napi_env env, napi_callback_info info) {
  size_t argc = 8;
  napi_value argv[8] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 6) return nullptr;
  const std::string algo = ValueToUtf8(env, argv[0]);
  uint8_t *key = nullptr, *iv = nullptr, *input = nullptr, *aad = nullptr, *auth_tag = nullptr;
  size_t key_len = 0, iv_len = 0, in_len = 0, aad_len = 0, auth_tag_len = 0;
  if (!GetBufferBytes(env, argv[1], &key, &key_len) ||
      !GetBufferBytes(env, argv[2], &iv, &iv_len) ||
      !GetBufferBytes(env, argv[3], &input, &in_len) ||
      !GetBufferBytes(env, argv[5], &aad, &aad_len)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "aead arguments must be Buffers");
    return nullptr;
  }
  bool decrypt = false;
  napi_get_value_bool(env, argv[4], &decrypt);
  if (argc >= 7 && argv[6] != nullptr) {
    napi_valuetype tag_type = napi_undefined;
    napi_typeof(env, argv[6], &tag_type);
    if (tag_type != napi_undefined && tag_type != napi_null) {
      if (!GetBufferBytes(env, argv[6], &auth_tag, &auth_tag_len)) {
        ThrowError(env, "ERR_INVALID_ARG_TYPE", "auth tag must be a Buffer");
        return nullptr;
      }
    }
  }
  int32_t requested_tag_len = 16;
  if (argc >= 8 && argv[7] != nullptr) {
    napi_get_value_int32(env, argv[7], &requested_tag_len);
    if (requested_tag_len <= 0 || requested_tag_len > 16) requested_tag_len = 16;
  }

  const EVP_CIPHER* cipher = EVP_get_cipherbyname(algo.c_str());
  if (cipher == nullptr) {
    ThrowError(env, "ERR_CRYPTO_UNKNOWN_CIPHER", "Unknown cipher");
    return nullptr;
  }
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (ctx == nullptr) {
    ThrowError(env, "ERR_CRYPTO_OPERATION_FAILED", "Failed to create cipher context");
    return nullptr;
  }
  int ok = EVP_CipherInit_ex(ctx, cipher, nullptr, nullptr, nullptr, decrypt ? 0 : 1);
  if (ok == 1) ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, static_cast<int>(iv_len), nullptr);
  if (ok == 1) ok = EVP_CipherInit_ex(ctx, nullptr, nullptr, key, iv, decrypt ? 0 : 1);
  int tmp_len = 0;
  if (ok == 1 && aad_len > 0) ok = EVP_CipherUpdate(ctx, nullptr, &tmp_len, aad, static_cast<int>(aad_len));
  if (ok == 1 && decrypt && auth_tag != nullptr) {
    ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, static_cast<int>(auth_tag_len), auth_tag);
  }
  std::vector<uint8_t> out(in_len + 32);
  int out_len = 0;
  if (ok == 1) ok = EVP_CipherUpdate(ctx, out.data(), &out_len, input, static_cast<int>(in_len));
  int final_len = 0;
  if (ok == 1) ok = EVP_CipherFinal_ex(ctx, out.data() + out_len, &final_len);
  out.resize(static_cast<size_t>(out_len + final_len));

  std::vector<uint8_t> out_tag;
  if (ok == 1 && !decrypt) {
    out_tag.resize(static_cast<size_t>(requested_tag_len));
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, requested_tag_len, out_tag.data()) != 1) {
      out_tag.clear();
    }
  }
  EVP_CIPHER_CTX_free(ctx);
  if (ok != 1) {
    ThrowLastOpenSslError(env, "ERR_CRYPTO_OPERATION_FAILED", "AEAD cipher operation failed");
    return nullptr;
  }
  napi_value result = nullptr;
  napi_create_object(env, &result);
  napi_value out_v = CreateBufferCopy(env, out.data(), out.size());
  napi_set_named_property(env, result, "output", out_v);
  napi_value tag_v = out_tag.empty() ? nullptr : CreateBufferCopy(env, out_tag.data(), out_tag.size());
  if (tag_v == nullptr) {
    napi_get_null(env, &tag_v);
  }
  napi_set_named_property(env, result, "authTag", tag_v);
  return result;
}

napi_value CryptoSignOneShot(napi_env env, napi_callback_info info) {
  size_t argc = 10;
  napi_value argv[10] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 3) return nullptr;
  const bool extended_key_args = argc >= 10;

  bool null_digest = false;
  napi_valuetype digest_type = napi_undefined;
  if (napi_typeof(env, argv[0], &digest_type) == napi_ok) {
    null_digest = (digest_type == napi_null || digest_type == napi_undefined);
  }
  const std::string algo = null_digest ? std::string() : ValueToUtf8(env, argv[0]);
  uint8_t* data = nullptr;
  size_t data_len = 0;
  std::vector<uint8_t> key_owned;
  uint8_t* key_bytes = nullptr;
  size_t key_len = 0;
  napi_value key_value = argv[2];
  if (!GetBufferBytes(env, argv[1], &data, &data_len) ||
      !GetKeyBytes(env, key_value, &key_owned, &key_bytes, &key_len)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "data and key must be Buffers or strings");
    return nullptr;
  }

  std::string passphrase;
  bool has_passphrase = false;
  int32_t key_format = -1;
  if (extended_key_args && argc >= 4 && argv[3] != nullptr) {
    napi_valuetype key_format_type = napi_undefined;
    if (napi_typeof(env, argv[3], &key_format_type) == napi_ok && key_format_type == napi_number) {
      napi_get_value_int32(env, argv[3], &key_format);
    }
  }
  if (extended_key_args && argc >= 6 && !IsNullOrUndefined(env, argv[5])) {
    if (!ReadPassphrase(env, argv[5], &passphrase, &has_passphrase)) {
      ThrowError(env, "ERR_INVALID_ARG_TYPE", "Invalid key passphrase");
      return nullptr;
    }
  }

  int32_t padding = 0;
  int32_t salt_len = INT32_MIN;
  int32_t dsa_sig_enc = 0;
  napi_valuetype padding_type = napi_undefined;
  napi_valuetype salt_type = napi_undefined;
  napi_value padding_arg = extended_key_args ? argv[6] : (argc >= 4 ? argv[3] : nullptr);
  napi_value salt_arg = extended_key_args ? argv[7] : (argc >= 5 ? argv[4] : nullptr);
  napi_value dsa_sig_enc_arg = extended_key_args ? argv[8] : nullptr;
  if (padding_arg != nullptr &&
      napi_typeof(env, padding_arg, &padding_type) == napi_ok &&
      padding_type == napi_number) {
    napi_get_value_int32(env, padding_arg, &padding);
  }
  if (salt_arg != nullptr &&
      napi_typeof(env, salt_arg, &salt_type) == napi_ok &&
      salt_type == napi_number) {
    napi_get_value_int32(env, salt_arg, &salt_len);
  }
  if (dsa_sig_enc_arg != nullptr) {
    napi_valuetype dsa_sig_enc_type = napi_undefined;
    if (napi_typeof(env, dsa_sig_enc_arg, &dsa_sig_enc_type) == napi_ok &&
        dsa_sig_enc_type == napi_number) {
      napi_get_value_int32(env, dsa_sig_enc_arg, &dsa_sig_enc);
    }
  }
  uint8_t* context = nullptr;
  size_t context_len = 0;
  bool has_context = false;
  napi_value context_arg = extended_key_args ? argv[9] : (argc >= 6 ? argv[5] : nullptr);
  if (context_arg != nullptr) {
    napi_valuetype context_type = napi_undefined;
    if (napi_typeof(env, context_arg, &context_type) == napi_ok &&
        context_type != napi_null &&
        context_type != napi_undefined) {
      if (!GetBufferBytes(env, context_arg, &context, &context_len)) {
        ThrowError(env, "ERR_INVALID_ARG_TYPE", "context must be a Buffer");
        return nullptr;
      }
      has_context = true;
    }
  }

  EVP_PKEY* pkey = ParsePrivateKeyWithPassphrase(key_bytes, key_len, passphrase, has_passphrase);
  if (pkey == nullptr) {
    if (!has_passphrase && key_format == 0) {
      napi_throw_type_error(env, "ERR_MISSING_PASSPHRASE", "Passphrase required for encrypted key");
      return nullptr;
    }
    ThrowLastOpenSslError(env, "ERR_CRYPTO_OPERATION_FAILED", "sign failed");
    return nullptr;
  }
  const ncrypto::Digest md = null_digest ? ncrypto::Digest(nullptr) : ResolveDigest(algo);
  if (!null_digest && !md) {
    EVP_PKEY_free(pkey);
    ThrowError(env, "ERR_CRYPTO_INVALID_DIGEST", "Invalid digest");
    return nullptr;
  }

  EVP_MD_CTX* mctx = EVP_MD_CTX_new();
  if (mctx == nullptr) {
    EVP_PKEY_free(pkey);
    ThrowError(env, "ERR_CRYPTO_OPERATION_FAILED", "Failed to create digest context");
    return nullptr;
  }
  const int pkey_type = EVP_PKEY_base_id(pkey);
  const bool is_rsa_family = (pkey_type == EVP_PKEY_RSA || pkey_type == EVP_PKEY_RSA_PSS);
  const bool is_ed_key = (pkey_type == EVP_PKEY_ED25519 || pkey_type == EVP_PKEY_ED448);
  const bool is_ed448 = (pkey_type == EVP_PKEY_ED448);
  if (has_context && context_len > 255) {
    EVP_MD_CTX_free(mctx);
    EVP_PKEY_free(pkey);
    ThrowError(env, "ERR_OUT_OF_RANGE", "context string must be at most 255 bytes");
    return nullptr;
  }
  if (has_context && context_len > 0 && !is_ed448) {
    EVP_MD_CTX_free(mctx);
    EVP_PKEY_free(pkey);
    ThrowError(env, "ERR_CRYPTO_UNSUPPORTED_OPERATION", "Context parameter is unsupported");
    return nullptr;
  }
  EVP_PKEY_CTX* pctx = nullptr;
  bool ok = false;
#ifdef OSSL_SIGNATURE_PARAM_CONTEXT_STRING
  if (has_context && context_len > 0) {
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_octet_string(
            OSSL_SIGNATURE_PARAM_CONTEXT_STRING,
            const_cast<unsigned char*>(context),
            context_len),
        OSSL_PARAM_END};
    ok = EVP_DigestSignInit_ex(
             mctx,
             &pctx,
             nullptr,
             nullptr,
             nullptr,
             pkey,
             params) == 1;
    if (!ok) {
      EVP_MD_CTX_free(mctx);
      EVP_PKEY_free(pkey);
      ThrowError(env, "ERR_CRYPTO_UNSUPPORTED_OPERATION", "Context parameter is unsupported");
      return nullptr;
    }
  } else {
    ok = EVP_DigestSignInit(mctx, &pctx, null_digest ? nullptr : md.get(), nullptr, pkey) == 1;
  }
#else
  if (has_context && context_len > 0) {
    EVP_MD_CTX_free(mctx);
    EVP_PKEY_free(pkey);
    ThrowError(env, "ERR_CRYPTO_UNSUPPORTED_OPERATION", "Context parameter is unsupported");
    return nullptr;
  }
  ok = EVP_DigestSignInit(mctx, &pctx, null_digest ? nullptr : md.get(), nullptr, pkey) == 1;
#endif
  int effective_padding = padding;
  if (is_rsa_family && effective_padding == 0 && pkey_type == EVP_PKEY_RSA_PSS) {
    // RSA-PSS keys require PSS padding even when callers omit padding.
    effective_padding = RSA_PKCS1_PSS_PADDING;
  }
  if (ok && pctx != nullptr && is_rsa_family && effective_padding != 0) {
    ok = EVP_PKEY_CTX_set_rsa_padding(pctx, effective_padding) == 1;
    if (ok && effective_padding == RSA_PKCS1_PSS_PADDING && salt_len != INT32_MIN) {
      ok = EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, salt_len) == 1;
    }
  }
  if (ok && is_ed_key && !null_digest) {
    ok = false;
    ThrowError(env, "ERR_CRYPTO_UNSUPPORTED_OPERATION", "Unsupported crypto operation");
  }
  size_t sig_len = 0;
  std::vector<uint8_t> sig;
  if (ok && is_ed_key) {
    ok = EVP_DigestSign(mctx, nullptr, &sig_len, data, data_len) == 1;
    if (ok) {
      sig.resize(sig_len);
      ok = EVP_DigestSign(mctx, sig.data(), &sig_len, data, data_len) == 1;
    }
  } else {
    if (ok) ok = EVP_DigestSignUpdate(mctx, data, data_len) == 1;
    if (ok) ok = EVP_DigestSignFinal(mctx, nullptr, &sig_len) == 1;
    if (ok) {
      sig.resize(sig_len);
      ok = EVP_DigestSignFinal(mctx, sig.data(), &sig_len) == 1;
    }
  }
  EVP_MD_CTX_free(mctx);

  if (!ok) {
    EVP_PKEY_free(pkey);
    ThrowLastOpenSslError(env, "ERR_CRYPTO_OPERATION_FAILED", "sign failed");
    return nullptr;
  }

  if (dsa_sig_enc == 1 && (pkey_type == EVP_PKEY_EC || pkey_type == EVP_PKEY_DSA)) {
    const size_t part_len = GetDsaSigPartLengthFromPkey(pkey);
    std::vector<uint8_t> p1363;
    if (part_len == 0 || !DerSignatureToP1363(sig.data(), sig_len, part_len, &p1363)) {
      EVP_PKEY_free(pkey);
      ThrowError(env, "ERR_CRYPTO_OPERATION_FAILED", "Malformed signature");
      return nullptr;
    }
    sig = std::move(p1363);
    sig_len = sig.size();
  }
  EVP_PKEY_free(pkey);
  return CreateBufferCopy(env, sig.data(), sig_len);
}

napi_value CryptoVerifyOneShot(napi_env env, napi_callback_info info) {
  size_t argc = 11;
  napi_value argv[11] = {
      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 4) return nullptr;
  const bool extended_key_args = argc >= 11;

  bool null_digest = false;
  napi_valuetype digest_type = napi_undefined;
  if (napi_typeof(env, argv[0], &digest_type) == napi_ok) {
    null_digest = (digest_type == napi_null || digest_type == napi_undefined);
  }
  const std::string algo = null_digest ? std::string() : ValueToUtf8(env, argv[0]);
  uint8_t* data = nullptr;
  size_t data_len = 0;
  std::vector<uint8_t> key_owned;
  uint8_t* key_bytes = nullptr;
  size_t key_len = 0;
  uint8_t* sig = nullptr;
  size_t sig_len = 0;
  std::vector<uint8_t> signature_storage;
  napi_value key_value = argv[2];
  napi_value signature_value = extended_key_args ? argv[6] : argv[3];
  if (!GetBufferBytes(env, argv[1], &data, &data_len) ||
      !GetKeyBytes(env, key_value, &key_owned, &key_bytes, &key_len) ||
      !GetBufferBytes(env, signature_value, &sig, &sig_len)) {
    ThrowError(env, "ERR_INVALID_ARG_TYPE", "data, key and signature must be Buffers or strings");
    return nullptr;
  }
  std::string passphrase;
  bool has_passphrase = false;
  int32_t key_format = -1;
  if (extended_key_args && argc >= 4 && argv[3] != nullptr) {
    napi_valuetype key_format_type = napi_undefined;
    if (napi_typeof(env, argv[3], &key_format_type) == napi_ok && key_format_type == napi_number) {
      napi_get_value_int32(env, argv[3], &key_format);
    }
  }
  if (extended_key_args && !IsNullOrUndefined(env, argv[5])) {
    if (!ReadPassphrase(env, argv[5], &passphrase, &has_passphrase)) {
      ThrowError(env, "ERR_INVALID_ARG_TYPE", "Invalid key passphrase");
      return nullptr;
    }
  }
  int32_t padding = 0;
  int32_t salt_len = INT32_MIN;
  int32_t dsa_sig_enc = 0;
  napi_valuetype padding_type = napi_undefined;
  napi_valuetype salt_type = napi_undefined;
  napi_value padding_arg = extended_key_args ? argv[7] : (argc >= 5 ? argv[4] : nullptr);
  napi_value salt_arg = extended_key_args ? argv[8] : (argc >= 6 ? argv[5] : nullptr);
  napi_value dsa_sig_enc_arg = extended_key_args ? argv[9] : nullptr;
  if (padding_arg != nullptr &&
      napi_typeof(env, padding_arg, &padding_type) == napi_ok &&
      padding_type == napi_number) {
    napi_get_value_int32(env, padding_arg, &padding);
  }
  if (salt_arg != nullptr &&
      napi_typeof(env, salt_arg, &salt_type) == napi_ok &&
      salt_type == napi_number) {
    napi_get_value_int32(env, salt_arg, &salt_len);
  }
  if (dsa_sig_enc_arg != nullptr) {
    napi_valuetype dsa_sig_enc_type = napi_undefined;
    if (napi_typeof(env, dsa_sig_enc_arg, &dsa_sig_enc_type) == napi_ok &&
        dsa_sig_enc_type == napi_number) {
      napi_get_value_int32(env, dsa_sig_enc_arg, &dsa_sig_enc);
    }
  }
  uint8_t* context = nullptr;
  size_t context_len = 0;
  bool has_context = false;
  napi_value context_arg = extended_key_args ? argv[10] : (argc >= 7 ? argv[6] : nullptr);
  if (context_arg != nullptr) {
    napi_valuetype context_type = napi_undefined;
    if (napi_typeof(env, context_arg, &context_type) == napi_ok &&
        context_type != napi_null &&
        context_type != napi_undefined) {
      if (!GetBufferBytes(env, context_arg, &context, &context_len)) {
        ThrowError(env, "ERR_INVALID_ARG_TYPE", "context must be a Buffer");
        return nullptr;
      }
      has_context = true;
    }
  }

  EVP_PKEY* pkey = ParsePublicKeyOrCert(key_bytes, key_len);
  if (pkey == nullptr) pkey = ParsePrivateKeyWithPassphrase(key_bytes, key_len, passphrase, has_passphrase);
  if (pkey == nullptr) {
    if (!has_passphrase && key_format == 0) {
      napi_throw_type_error(env, "ERR_MISSING_PASSPHRASE", "Passphrase required for encrypted key");
      return nullptr;
    }
    ThrowLastOpenSslError(env, "ERR_CRYPTO_OPERATION_FAILED", "verify failed");
    return nullptr;
  }
  const ncrypto::Digest md = null_digest ? ncrypto::Digest(nullptr) : ResolveDigest(algo);
  if (!null_digest && !md) {
    EVP_PKEY_free(pkey);
    ThrowError(env, "ERR_CRYPTO_INVALID_DIGEST", "Invalid digest");
    return nullptr;
  }

  EVP_MD_CTX* mctx = EVP_MD_CTX_new();
  if (mctx == nullptr) {
    EVP_PKEY_free(pkey);
    ThrowError(env, "ERR_CRYPTO_OPERATION_FAILED", "Failed to create digest context");
    return nullptr;
  }
  const int pkey_type = EVP_PKEY_base_id(pkey);
  const bool is_rsa_family = (pkey_type == EVP_PKEY_RSA || pkey_type == EVP_PKEY_RSA_PSS);
  const bool is_ed_key = (pkey_type == EVP_PKEY_ED25519 || pkey_type == EVP_PKEY_ED448);
  const bool is_ed448 = (pkey_type == EVP_PKEY_ED448);
  if (dsa_sig_enc == 1 && (pkey_type == EVP_PKEY_EC || pkey_type == EVP_PKEY_DSA)) {
    const size_t part_len = GetDsaSigPartLengthFromPkey(pkey);
    if (part_len == 0 || sig_len != part_len * 2 ||
        !P1363ToDerSignature(pkey_type, sig, sig_len, part_len, &signature_storage)) {
      EVP_MD_CTX_free(mctx);
      EVP_PKEY_free(pkey);
      napi_value out = nullptr;
      napi_get_boolean(env, false, &out);
      return out != nullptr ? out : nullptr;
    }
    sig = signature_storage.data();
    sig_len = signature_storage.size();
  }
  if (has_context && context_len > 255) {
    EVP_MD_CTX_free(mctx);
    EVP_PKEY_free(pkey);
    ThrowError(env, "ERR_OUT_OF_RANGE", "context string must be at most 255 bytes");
    return nullptr;
  }
  if (has_context && context_len > 0 && !is_ed448) {
    EVP_MD_CTX_free(mctx);
    EVP_PKEY_free(pkey);
    ThrowError(env, "ERR_CRYPTO_UNSUPPORTED_OPERATION", "Context parameter is unsupported");
    return nullptr;
  }
  EVP_PKEY_CTX* pctx = nullptr;
  bool ok = false;
#ifdef OSSL_SIGNATURE_PARAM_CONTEXT_STRING
  if (has_context && context_len > 0) {
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_octet_string(
            OSSL_SIGNATURE_PARAM_CONTEXT_STRING,
            const_cast<unsigned char*>(context),
            context_len),
        OSSL_PARAM_END};
    ok = EVP_DigestVerifyInit_ex(
             mctx,
             &pctx,
             nullptr,
             nullptr,
             nullptr,
             pkey,
             params) == 1;
    if (!ok) {
      EVP_MD_CTX_free(mctx);
      EVP_PKEY_free(pkey);
      ThrowError(env, "ERR_CRYPTO_UNSUPPORTED_OPERATION", "Context parameter is unsupported");
      return nullptr;
    }
  } else {
    ok = EVP_DigestVerifyInit(mctx, &pctx, null_digest ? nullptr : md.get(), nullptr, pkey) == 1;
  }
#else
  if (has_context && context_len > 0) {
    EVP_MD_CTX_free(mctx);
    EVP_PKEY_free(pkey);
    ThrowError(env, "ERR_CRYPTO_UNSUPPORTED_OPERATION", "Context parameter is unsupported");
    return nullptr;
  }
  ok = EVP_DigestVerifyInit(mctx, &pctx, null_digest ? nullptr : md.get(), nullptr, pkey) == 1;
#endif
  int effective_padding = padding;
  if (is_rsa_family && effective_padding == 0 && pkey_type == EVP_PKEY_RSA_PSS) {
    effective_padding = RSA_PKCS1_PSS_PADDING;
  }
  if (ok && pctx != nullptr && is_rsa_family && effective_padding != 0) {
    ok = EVP_PKEY_CTX_set_rsa_padding(pctx, effective_padding) == 1;
    if (ok && effective_padding == RSA_PKCS1_PSS_PADDING && salt_len != INT32_MIN) {
      ok = EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, salt_len) == 1;
    }
  }
  int vr = 0;
  if (ok && is_ed_key && !null_digest) {
    ok = false;
    ThrowError(env, "ERR_CRYPTO_UNSUPPORTED_OPERATION", "Unsupported crypto operation");
  }
  if (ok && is_ed_key) {
    vr = EVP_DigestVerify(mctx, sig, sig_len, data, data_len);
  } else {
    if (ok) ok = EVP_DigestVerifyUpdate(mctx, data, data_len) == 1;
    vr = ok ? EVP_DigestVerifyFinal(mctx, sig, sig_len) : 0;
  }
  EVP_MD_CTX_free(mctx);
  EVP_PKEY_free(pkey);

  if (!ok) {
    bool pending = false;
    if (napi_is_exception_pending(env, &pending) != napi_ok || !pending) {
      ThrowLastOpenSslError(env, "ERR_CRYPTO_OPERATION_FAILED", "verify failed");
    }
    return nullptr;
  }
  if (vr != 1) ERR_clear_error();

  napi_value out = nullptr;
  napi_get_boolean(env, vr == 1, &out);
  return out;
}

void SetMethod(napi_env env, napi_value obj, const char* name, napi_callback fn) {
  napi_value method = nullptr;
  if (napi_create_function(env, name, NAPI_AUTO_LENGTH, fn, nullptr, &method) == napi_ok && method != nullptr) {
    napi_set_named_property(env, obj, name, method);
  }
}

}  // namespace

napi_value InstallCryptoBinding(napi_env env) {
  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) return nullptr;

  SetMethod(env, binding, "hashOneShot", CryptoHashOneShot);
  SetMethod(env, binding, "hashOneShotXof", CryptoHashOneShotXof);
  SetMethod(env, binding, "hmacOneShot", CryptoHmacOneShot);
  SetMethod(env, binding, "randomFillSync", CryptoRandomFillSync);
  SetMethod(env, binding, "randomBytes", CryptoRandomBytes);
  SetMethod(env, binding, "pbkdf2Sync", CryptoPbkdf2Sync);
  SetMethod(env, binding, "scryptSync", CryptoScryptSync);
  SetMethod(env, binding, "hkdfSync", CryptoHkdfSync);
  SetMethod(env, binding, "cipherTransform", CryptoCipherTransform);
  SetMethod(env, binding, "getHashes", CryptoGetHashes);
  SetMethod(env, binding, "getCiphers", CryptoGetCiphers);
  SetMethod(env, binding, "getSSLCiphers", CryptoGetSSLCiphers);
  SetMethod(env, binding, "getCurves", CryptoGetCurves);
  SetMethod(env, binding, "getCipherInfo", CryptoGetCipherInfo);
  SetMethod(env, binding, "parsePfx", CryptoParsePfx);
  SetMethod(env, binding, "parseCrl", CryptoParseCrl);
  SetMethod(env, binding, "secureContextCreate", CryptoSecureContextCreate);
  SetMethod(env, binding, "secureContextInit", CryptoSecureContextInit);
  SetMethod(env, binding, "secureContextSetOptions", CryptoSecureContextSetOptions);
  SetMethod(env, binding, "secureContextSetCiphers", CryptoSecureContextSetCiphers);
  SetMethod(env, binding, "secureContextSetCipherSuites", CryptoSecureContextSetCipherSuites);
  SetMethod(env, binding, "secureContextSetCert", CryptoSecureContextSetCert);
  SetMethod(env, binding, "secureContextSetKey", CryptoSecureContextSetKey);
  SetMethod(env, binding, "secureContextAddCACert", CryptoSecureContextAddCACert);
  SetMethod(env, binding, "secureContextAddCrl", CryptoSecureContextAddCrl);
  SetMethod(env, binding, "signOneShot", CryptoSignOneShot);
  SetMethod(env, binding, "verifyOneShot", CryptoVerifyOneShot);
  SetMethod(env, binding, "getAsymmetricKeyDetails", CryptoGetAsymmetricKeyDetails);
  SetMethod(env, binding, "getAsymmetricKeyType", CryptoGetAsymmetricKeyType);
  SetMethod(env, binding, "publicEncrypt", CryptoPublicEncrypt);
  SetMethod(env, binding, "privateDecrypt", CryptoPrivateDecrypt);
  SetMethod(env, binding, "cipherTransformAead", CryptoCipherTransformAead);

  return binding;
}

}  // namespace ubi::crypto
