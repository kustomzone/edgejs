#include "ubi_tls_wrap.h"

#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <uv.h>

#include "crypto/ubi_secure_context_bridge.h"
#include "ncrypto.h"
#include "ubi_async_wrap.h"
#include "ubi_runtime.h"
#include "ubi_stream_base.h"
#include "ubi_stream_wrap.h"

namespace {

struct PendingAppWrite {
  napi_ref req_ref = nullptr;
  std::vector<uint8_t> data;
};

struct PendingEncryptedWrite {
  std::vector<uint8_t> data;
  napi_ref completion_req_ref = nullptr;
};

struct TlsWrap {
  napi_env env = nullptr;
  napi_ref wrapper_ref = nullptr;
  napi_ref parent_ref = nullptr;
  napi_ref context_ref = nullptr;
  napi_ref pending_shutdown_req_ref = nullptr;
  bool is_server = false;
  bool started = false;
  bool established = false;
  bool eof = false;
  bool parent_write_in_progress = false;
  bool has_active_write_issued_by_prev_listener = false;
  bool waiting_cert_cb = false;
  bool cert_cb_running = false;
  bool alpn_callback_enabled = false;
  bool session_callbacks_enabled = false;
  bool request_cert = false;
  bool reject_unauthorized = false;
  bool shutdown_started = false;
  int64_t async_id = 0;
  int cycle_depth = 0;
  SSL* ssl = nullptr;
  BIO* enc_in = nullptr;
  BIO* enc_out = nullptr;
  ubi::crypto::SecureContextHolder* secure_context = nullptr;
  std::deque<PendingAppWrite> pending_app_writes;
  std::deque<PendingEncryptedWrite> pending_encrypted_writes;
  std::vector<uint8_t> pending_session;
  std::vector<unsigned char> alpn_protos;
};

struct TlsBindingState {
  napi_ref binding_ref = nullptr;
  napi_ref tls_wrap_ctor_ref = nullptr;
  int64_t next_async_id = 300000;
  std::vector<TlsWrap*> wraps;
};

std::unordered_map<napi_env, TlsBindingState> g_tls_states;
std::unordered_set<napi_env> g_tls_cleanup_hook_registered;

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

void DeleteRefIfPresent(napi_env env, napi_ref* ref) {
  if (env == nullptr || ref == nullptr || *ref == nullptr) return;
  napi_delete_reference(env, *ref);
  *ref = nullptr;
}

napi_value GetRefValue(napi_env env, napi_ref ref) {
  if (env == nullptr || ref == nullptr) return nullptr;
  napi_value out = nullptr;
  if (napi_get_reference_value(env, ref, &out) != napi_ok) return nullptr;
  return out;
}

napi_value GetNamedValue(napi_env env, napi_value obj, const char* key) {
  if (env == nullptr || obj == nullptr || key == nullptr) return nullptr;
  napi_value out = nullptr;
  if (napi_get_named_property(env, obj, key, &out) != napi_ok) return nullptr;
  return out;
}

void InvokeReqWithStatus(TlsWrap* wrap, napi_ref* req_ref, int status) {
  if (wrap == nullptr || wrap->env == nullptr || req_ref == nullptr || *req_ref == nullptr) return;
  napi_value req_obj = GetRefValue(wrap->env, *req_ref);
  napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
  if (req_obj != nullptr && status < 0) {
    UbiStreamBaseSetReqError(wrap->env, req_obj, status);
  }
  napi_value argv[3] = {
      MakeInt32(wrap->env, status),
      self != nullptr ? self : Undefined(wrap->env),
      status < 0 && req_obj != nullptr ? GetNamedValue(wrap->env, req_obj, "error") : Undefined(wrap->env),
  };
  if (req_obj != nullptr) {
    UbiStreamBaseInvokeReqOnComplete(wrap->env, req_obj, status, argv, 3);
  }
  DeleteRefIfPresent(wrap->env, req_ref);
}

bool IsFunction(napi_env env, napi_value value) {
  napi_valuetype type = napi_undefined;
  return value != nullptr && napi_typeof(env, value, &type) == napi_ok && type == napi_function;
}

std::string ValueToUtf8(napi_env env, napi_value value) {
  if (value == nullptr) return "";
  size_t len = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &len) != napi_ok) return "";
  std::string out(len + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, value, out.data(), out.size(), &copied) != napi_ok) return "";
  out.resize(copied);
  return out;
}

void SetState(napi_env env, int idx, int32_t value) {
  int32_t* state = UbiGetStreamBaseState(env);
  if (state == nullptr) return;
  state[idx] = value;
}

TlsBindingState& EnsureState(napi_env env) {
  if (env != nullptr) {
    auto [it, inserted] = g_tls_cleanup_hook_registered.emplace(env);
    if (inserted && napi_add_env_cleanup_hook(
                        env,
                        [](void* data) {
                          napi_env cleanup_env = static_cast<napi_env>(data);
                          g_tls_cleanup_hook_registered.erase(cleanup_env);

                          auto state_it = g_tls_states.find(cleanup_env);
                          if (state_it == g_tls_states.end()) return;
                          for (TlsWrap* wrap : state_it->second.wraps) {
                            if (wrap == nullptr) continue;
                            DeleteRefIfPresent(cleanup_env, &wrap->wrapper_ref);
                            DeleteRefIfPresent(cleanup_env, &wrap->parent_ref);
                            DeleteRefIfPresent(cleanup_env, &wrap->context_ref);
                            DeleteRefIfPresent(cleanup_env, &wrap->pending_shutdown_req_ref);
                            for (auto& pending : wrap->pending_app_writes) {
                              DeleteRefIfPresent(cleanup_env, &pending.req_ref);
                            }
                            for (auto& pending : wrap->pending_encrypted_writes) {
                              DeleteRefIfPresent(cleanup_env, &pending.completion_req_ref);
                            }
                          }
                          DeleteRefIfPresent(cleanup_env, &state_it->second.binding_ref);
                          DeleteRefIfPresent(cleanup_env, &state_it->second.tls_wrap_ctor_ref);
                          state_it->second.wraps.clear();
                          g_tls_states.erase(state_it);
                        },
                        env) != napi_ok) {
      g_tls_cleanup_hook_registered.erase(it);
    }
  }
  return g_tls_states[env];
}

TlsWrap* UnwrapTlsWrap(napi_env env, napi_value value) {
  if (env == nullptr || value == nullptr) return nullptr;
  TlsWrap* wrap = nullptr;
  if (napi_unwrap(env, value, reinterpret_cast<void**>(&wrap)) != napi_ok) return nullptr;
  return wrap;
}

TlsWrap* FindWrapByParent(napi_env env, napi_value parent) {
  auto it = g_tls_states.find(env);
  if (it == g_tls_states.end()) return nullptr;
  for (TlsWrap* wrap : it->second.wraps) {
    if (wrap == nullptr) continue;
    napi_value candidate = GetRefValue(env, wrap->parent_ref);
    bool same = false;
    if (candidate != nullptr && napi_strict_equals(env, candidate, parent, &same) == napi_ok && same) {
      return wrap;
    }
  }
  return nullptr;
}

TlsWrap* UnwrapThis(napi_env env,
                    napi_callback_info info,
                    size_t* argc,
                    napi_value* argv,
                    napi_value* self_out) {
  size_t local_argc = argc != nullptr ? *argc : 0;
  napi_value self = nullptr;
  if (napi_get_cb_info(env, info, &local_argc, argv, &self, nullptr) != napi_ok) return nullptr;
  if (argc != nullptr) *argc = local_argc;
  if (self_out != nullptr) *self_out = self;
  return UnwrapTlsWrap(env, self);
}

void RemoveWrapFromState(TlsWrap* wrap) {
  if (wrap == nullptr || wrap->env == nullptr) return;
  auto it = g_tls_states.find(wrap->env);
  if (it == g_tls_states.end()) return;
  auto& wraps = it->second.wraps;
  for (auto vec_it = wraps.begin(); vec_it != wraps.end(); ++vec_it) {
    if (*vec_it == wrap) {
      wraps.erase(vec_it);
      break;
    }
  }
}

std::vector<uint8_t> ReadAllPendingBio(BIO* bio) {
  std::vector<uint8_t> out;
  if (bio == nullptr) return out;
  const size_t pending = static_cast<size_t>(BIO_ctrl_pending(bio));
  if (pending == 0) return out;
  out.resize(pending);
  const int read = BIO_read(bio, out.data(), static_cast<int>(pending));
  if (read <= 0) {
    out.clear();
    return out;
  }
  out.resize(static_cast<size_t>(read));
  return out;
}

napi_value CreateBufferCopy(napi_env env, const uint8_t* data, size_t len) {
  napi_value out = nullptr;
  void* copied = nullptr;
  if (napi_create_buffer_copy(env, len, len > 0 ? data : nullptr, &copied, &out) != napi_ok) return nullptr;
  return out;
}

void EmitOnReadData(TlsWrap* wrap, const uint8_t* data, size_t len) {
  if (wrap == nullptr || wrap->env == nullptr) return;
  napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
  napi_value onread = GetNamedValue(wrap->env, self, "onread");
  if (!IsFunction(wrap->env, onread)) return;
  napi_value arraybuffer = nullptr;
  void* raw = nullptr;
  if (napi_create_arraybuffer(wrap->env, len, &raw, &arraybuffer) != napi_ok || arraybuffer == nullptr) return;
  if (len > 0 && raw != nullptr) {
    std::memcpy(raw, data, len);
  }
  SetState(wrap->env, kUbiReadBytesOrError, static_cast<int32_t>(len));
  SetState(wrap->env, kUbiArrayBufferOffset, 0);
  napi_value argv[1] = {arraybuffer};
  napi_value ignored = nullptr;
  (void)UbiAsyncWrapMakeCallback(
      wrap->env, wrap->async_id, self, self, onread, 1, argv, &ignored, kUbiMakeCallbackNone);
}

void EmitOnReadStatus(TlsWrap* wrap, int32_t status) {
  if (wrap == nullptr || wrap->env == nullptr) return;
  napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
  napi_value onread = GetNamedValue(wrap->env, self, "onread");
  if (!IsFunction(wrap->env, onread)) return;
  SetState(wrap->env, kUbiReadBytesOrError, status);
  SetState(wrap->env, kUbiArrayBufferOffset, 0);
  napi_value argv[1] = {Undefined(wrap->env)};
  napi_value ignored = nullptr;
  (void)UbiAsyncWrapMakeCallback(
      wrap->env, wrap->async_id, self, self, onread, 1, argv, &ignored, kUbiMakeCallbackNone);
}

napi_value CreateErrorWithCode(napi_env env, const char* code, const std::string& message) {
  napi_value code_v = nullptr;
  napi_value msg_v = nullptr;
  napi_value err_v = nullptr;
  if (code != nullptr) {
    napi_create_string_utf8(env, code, NAPI_AUTO_LENGTH, &code_v);
  } else {
    napi_get_undefined(env, &code_v);
  }
  napi_create_string_utf8(env, message.c_str(), NAPI_AUTO_LENGTH, &msg_v);
  napi_create_error(env, code_v, msg_v, &err_v);
  if (err_v != nullptr && code != nullptr && code_v != nullptr) {
    napi_set_named_property(env, err_v, "code", code_v);
  }
  return err_v;
}

void SetErrorStringProperty(napi_env env, napi_value err, const char* name, const char* value) {
  if (err == nullptr || name == nullptr || value == nullptr || value[0] == '\0') return;
  napi_value out = nullptr;
  if (napi_create_string_utf8(env, value, NAPI_AUTO_LENGTH, &out) == napi_ok && out != nullptr) {
    napi_set_named_property(env, err, name, out);
  }
}

napi_value CreateLastOpenSslError(napi_env env, const char* fallback_code, const char* fallback_message) {
  unsigned long err = 0;
  while (true) {
    const unsigned long next = ERR_get_error();
    if (next == 0) break;
    err = next;
  }
  if (err == 0) return CreateErrorWithCode(env, fallback_code, fallback_message != nullptr ? fallback_message
                                                                                           : "OpenSSL error");
  char buf[256];
  ERR_error_string_n(err, buf, sizeof(buf));
  napi_value error = CreateErrorWithCode(env, fallback_code, buf);
  SetErrorStringProperty(env, error, "library", ERR_lib_error_string(err));
  SetErrorStringProperty(env, error, "reason", ERR_reason_error_string(err));
  return error;
}

void EmitError(TlsWrap* wrap, napi_value error) {
  if (wrap == nullptr || wrap->env == nullptr || error == nullptr) return;
  napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
  napi_value onerror = GetNamedValue(wrap->env, self, "onerror");
  if (!IsFunction(wrap->env, onerror)) return;
  napi_value argv[1] = {error};
  napi_value ignored = nullptr;
  (void)UbiAsyncWrapMakeCallback(
      wrap->env, wrap->async_id, self, self, onerror, 1, argv, &ignored, kUbiMakeCallbackNone);
}

void CompleteReq(TlsWrap* wrap, napi_ref* req_ref, int status) {
  if (wrap == nullptr || wrap->env == nullptr || req_ref == nullptr || *req_ref == nullptr) return;
  napi_value req_obj = GetRefValue(wrap->env, *req_ref);
  napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
  napi_value argv[3] = {
      MakeInt32(wrap->env, status),
      self != nullptr ? self : Undefined(wrap->env),
      status < 0 ? GetNamedValue(wrap->env, req_obj, "error") : Undefined(wrap->env),
  };
  UbiStreamBaseInvokeReqOnComplete(wrap->env, req_obj, status, argv, 3);
  DeleteRefIfPresent(wrap->env, req_ref);
}

bool GetArrayBufferBytes(napi_env env,
                         napi_value value,
                         const uint8_t** data,
                         size_t* len,
                         size_t* offset_out) {
  if (data == nullptr || len == nullptr || offset_out == nullptr) return false;
  *data = nullptr;
  *len = 0;
  *offset_out = 0;
  if (value == nullptr) return false;
  bool is_ab = false;
  if (napi_is_arraybuffer(env, value, &is_ab) == napi_ok && is_ab) {
    void* raw = nullptr;
    size_t byte_len = 0;
    if (napi_get_arraybuffer_info(env, value, &raw, &byte_len) != napi_ok || raw == nullptr) return false;
    *data = static_cast<const uint8_t*>(raw);
    *len = byte_len;
    return true;
  }
  bool is_typedarray = false;
  if (napi_is_typedarray(env, value, &is_typedarray) == napi_ok && is_typedarray) {
    napi_typedarray_type type = napi_uint8_array;
    size_t element_len = 0;
    void* raw = nullptr;
    napi_value ab = nullptr;
    size_t byte_offset = 0;
    if (napi_get_typedarray_info(env, value, &type, &element_len, &raw, &ab, &byte_offset) != napi_ok ||
        raw == nullptr) {
      return false;
    }
    *data = static_cast<const uint8_t*>(raw);
    *len = element_len * UbiTypedArrayElementSize(type);
    *offset_out = byte_offset;
    return true;
  }
  return false;
}

int32_t CallParentMethodInt(TlsWrap* wrap, const char* method, size_t argc, napi_value* argv, napi_value* result_out) {
  if (wrap == nullptr || method == nullptr) return UV_EINVAL;
  napi_value parent = GetRefValue(wrap->env, wrap->parent_ref);
  if (parent == nullptr) return UV_EINVAL;
  napi_value fn = GetNamedValue(wrap->env, parent, method);
  if (!IsFunction(wrap->env, fn)) return UV_EINVAL;
  napi_value result = nullptr;
  if (napi_call_function(wrap->env, parent, fn, argc, argv, &result) != napi_ok || result == nullptr) {
    return UV_EINVAL;
  }
  if (result_out != nullptr) *result_out = result;
  int32_t out = 0;
  if (napi_get_value_int32(wrap->env, result, &out) != napi_ok) return 0;
  return out;
}

bool SetSecureContextOnSsl(TlsWrap* wrap, ubi::crypto::SecureContextHolder* holder) {
  if (wrap == nullptr || wrap->ssl == nullptr || holder == nullptr || holder->ctx == nullptr) return false;
  if (SSL_set_SSL_CTX(wrap->ssl, holder->ctx) == nullptr) return false;
  wrap->secure_context = holder;
  X509_STORE* store = SSL_CTX_get_cert_store(holder->ctx);
  if (store != nullptr && SSL_set1_verify_cert_store(wrap->ssl, store) != 1) return false;
  STACK_OF(X509_NAME)* list = SSL_dup_CA_list(SSL_CTX_get_client_CA_list(holder->ctx));
  SSL_set_client_CA_list(wrap->ssl, list);
  return true;
}

void InitSsl(TlsWrap* wrap);
void Cycle(TlsWrap* wrap);
void TryStartParentWrite(TlsWrap* wrap);
void MaybeStartParentShutdown(TlsWrap* wrap);

void CleanupPendingWrites(TlsWrap* wrap, int status) {
  if (wrap == nullptr) return;
  while (!wrap->pending_encrypted_writes.empty()) {
    PendingEncryptedWrite pending = std::move(wrap->pending_encrypted_writes.front());
    wrap->pending_encrypted_writes.pop_front();
    CompleteReq(wrap, &pending.completion_req_ref, status);
  }
  while (!wrap->pending_app_writes.empty()) {
    PendingAppWrite pending = std::move(wrap->pending_app_writes.front());
    wrap->pending_app_writes.pop_front();
    CompleteReq(wrap, &pending.req_ref, status);
  }
  InvokeReqWithStatus(wrap, &wrap->pending_shutdown_req_ref, status);
}

void DestroySsl(TlsWrap* wrap) {
  if (wrap == nullptr || wrap->ssl == nullptr) return;
  CleanupPendingWrites(wrap, UV_ECANCELED);
  SSL_free(wrap->ssl);
  wrap->ssl = nullptr;
  wrap->enc_in = nullptr;
  wrap->enc_out = nullptr;
  wrap->parent_write_in_progress = false;
}

void TlsWrapFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* wrap = static_cast<TlsWrap*>(data);
  if (wrap == nullptr) return;
  DestroySsl(wrap);
  RemoveWrapFromState(wrap);
  DeleteRefIfPresent(env, &wrap->parent_ref);
  DeleteRefIfPresent(env, &wrap->context_ref);
  DeleteRefIfPresent(env, &wrap->wrapper_ref);
  delete wrap;
}

void EmitHandshakeCallback(TlsWrap* wrap, const char* name, size_t argc, napi_value* argv) {
  if (wrap == nullptr || wrap->env == nullptr) return;
  napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
  napi_value cb = GetNamedValue(wrap->env, self, name);
  if (!IsFunction(wrap->env, cb)) return;
  napi_value ignored = nullptr;
  (void)UbiAsyncWrapMakeCallback(
      wrap->env, wrap->async_id, self, self, cb, argc, argv, &ignored, kUbiMakeCallbackNone);
}

void SslInfoCallback(const SSL* ssl, int where, int /*ret*/) {
  if ((where & (SSL_CB_HANDSHAKE_START | SSL_CB_HANDSHAKE_DONE)) == 0) return;
  TlsWrap* wrap = static_cast<TlsWrap*>(SSL_get_app_data(ssl));
  if (wrap == nullptr) return;
  if ((where & SSL_CB_HANDSHAKE_START) != 0) {
    const int64_t now_ms = static_cast<int64_t>(uv_hrtime() / 1000000ULL);
    napi_value argv[1] = {MakeInt64(wrap->env, now_ms)};
    EmitHandshakeCallback(wrap, "onhandshakestart", 1, argv);
  }
  if ((where & SSL_CB_HANDSHAKE_DONE) != 0 && SSL_renegotiate_pending(const_cast<SSL*>(ssl)) == 0) {
    wrap->established = true;
    EmitHandshakeCallback(wrap, "onhandshakedone", 0, nullptr);
  }
}

void KeylogCallback(const SSL* ssl, const char* line) {
  TlsWrap* wrap = static_cast<TlsWrap*>(SSL_get_app_data(ssl));
  if (wrap == nullptr || line == nullptr) return;
  const size_t len = std::strlen(line);
  std::vector<uint8_t> bytes(len + 1, 0);
  if (len > 0) std::memcpy(bytes.data(), line, len);
  bytes[len] = '\n';
  napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
  napi_value cb = GetNamedValue(wrap->env, self, "onkeylog");
  if (!IsFunction(wrap->env, cb)) return;
  napi_value buffer = CreateBufferCopy(wrap->env, bytes.data(), bytes.size());
  if (buffer == nullptr) return;
  napi_value argv[1] = {buffer};
  napi_value ignored = nullptr;
  (void)UbiAsyncWrapMakeCallback(
      wrap->env, wrap->async_id, self, self, cb, 1, argv, &ignored, kUbiMakeCallbackNone);
}

int VerifyCallback(int /*preverify_ok*/, X509_STORE_CTX* /*ctx*/) {
  return 1;
}

int CertCallback(SSL* ssl, void* arg) {
  auto* wrap = static_cast<TlsWrap*>(arg);
  if (wrap == nullptr || !wrap->is_server || !wrap->waiting_cert_cb) return 1;
  if (wrap->cert_cb_running) return -1;

  wrap->cert_cb_running = true;
  napi_value info = nullptr;
  napi_create_object(wrap->env, &info);
  const char* servername = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
  if (servername != nullptr) {
    napi_value sn = nullptr;
    napi_create_string_utf8(wrap->env, servername, NAPI_AUTO_LENGTH, &sn);
    if (sn != nullptr) napi_set_named_property(wrap->env, info, "servername", sn);
  }
  napi_value ocsp = MakeBool(wrap->env, SSL_get_tlsext_status_type(ssl) == TLSEXT_STATUSTYPE_ocsp);
  if (ocsp != nullptr) napi_set_named_property(wrap->env, info, "OCSPRequest", ocsp);

  napi_value argv[1] = {info};
  EmitHandshakeCallback(wrap, "oncertcb", 1, argv);
  return wrap->cert_cb_running ? -1 : 1;
}

int SelectALPNCallback(SSL* ssl,
                       const unsigned char** out,
                       unsigned char* outlen,
                       const unsigned char* in,
                       unsigned int inlen,
                       void* /*arg*/) {
  auto* wrap = static_cast<TlsWrap*>(SSL_get_app_data(ssl));
  if (wrap == nullptr) return SSL_TLSEXT_ERR_NOACK;

  if (wrap->alpn_callback_enabled) {
    napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
    napi_value cb = GetNamedValue(wrap->env, self, "ALPNCallback");
    if (!IsFunction(wrap->env, cb)) return SSL_TLSEXT_ERR_ALERT_FATAL;
    napi_value buffer = CreateBufferCopy(wrap->env, in, inlen);
    napi_value argv[1] = {buffer};
    napi_value result = nullptr;
    if (UbiAsyncWrapMakeCallback(
            wrap->env, wrap->async_id, self, self, cb, 1, argv, &result, kUbiMakeCallbackNone) != napi_ok ||
        result == nullptr) {
      return SSL_TLSEXT_ERR_ALERT_FATAL;
    }
    napi_valuetype type = napi_undefined;
    if (napi_typeof(wrap->env, result, &type) != napi_ok || type == napi_undefined) {
      return SSL_TLSEXT_ERR_ALERT_FATAL;
    }
    uint32_t offset = 0;
    if (napi_get_value_uint32(wrap->env, result, &offset) != napi_ok || offset >= inlen) {
      return SSL_TLSEXT_ERR_ALERT_FATAL;
    }
    *outlen = *(in + offset);
    *out = in + offset + 1;
    return SSL_TLSEXT_ERR_OK;
  }

  if (wrap->alpn_protos.empty()) return SSL_TLSEXT_ERR_NOACK;
  const int rc =
      SSL_select_next_proto(const_cast<unsigned char**>(out),
                            outlen,
                            wrap->alpn_protos.data(),
                            static_cast<unsigned int>(wrap->alpn_protos.size()),
                            in,
                            inlen);
  return rc == OPENSSL_NPN_NEGOTIATED ? SSL_TLSEXT_ERR_OK : SSL_TLSEXT_ERR_ALERT_FATAL;
}

int NewSessionCallback(SSL* ssl, SSL_SESSION* session) {
  auto* wrap = static_cast<TlsWrap*>(SSL_get_app_data(ssl));
  if (wrap == nullptr || wrap->env == nullptr || session == nullptr) return 1;
  napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
  napi_value cb = GetNamedValue(wrap->env, self, "onnewsession");
  if (!IsFunction(wrap->env, cb)) return 1;

  unsigned int id_len = 0;
  const unsigned char* id = SSL_SESSION_get_id(session, &id_len);
  napi_value id_buffer = CreateBufferCopy(wrap->env, id, id_len);

  const int encoded_len = i2d_SSL_SESSION(session, nullptr);
  if (encoded_len <= 0 || id_buffer == nullptr) return 1;
  std::vector<uint8_t> encoded(static_cast<size_t>(encoded_len));
  unsigned char* ptr = encoded.data();
  if (i2d_SSL_SESSION(session, &ptr) != encoded_len) return 1;

  napi_value session_buffer = CreateBufferCopy(wrap->env, encoded.data(), encoded.size());
  if (session_buffer == nullptr) return 1;

  napi_value argv[2] = {id_buffer, session_buffer};
  napi_value ignored = nullptr;
  (void)UbiAsyncWrapMakeCallback(
      wrap->env, wrap->async_id, self, self, cb, 2, argv, &ignored, kUbiMakeCallbackNone);
  return 1;
}

bool HandleSslError(TlsWrap* wrap, int ssl_result, const char* fallback_code, const char* fallback_message) {
  if (wrap == nullptr || wrap->ssl == nullptr) return true;
  const int err = SSL_get_error(wrap->ssl, ssl_result);
  if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_X509_LOOKUP) {
    return false;
  }
  if (err == SSL_ERROR_ZERO_RETURN) return false;
  EmitError(wrap, CreateLastOpenSslError(wrap->env, fallback_code, fallback_message));
  return true;
}

void QueueEncryptedWrite(TlsWrap* wrap, std::vector<uint8_t> bytes, napi_ref completion_req_ref) {
  if (wrap == nullptr) return;
  if (bytes.empty()) {
    if (completion_req_ref != nullptr) {
      CompleteReq(wrap, &completion_req_ref, 0);
    }
    return;
  }
  PendingEncryptedWrite pending;
  pending.data = std::move(bytes);
  pending.completion_req_ref = completion_req_ref;
  wrap->pending_encrypted_writes.push_back(std::move(pending));
}

napi_value CreateInternalWriteReq(TlsWrap* wrap);

void OnInternalWriteDone(TlsWrap* wrap, int status) {
  if (wrap == nullptr) return;
  wrap->parent_write_in_progress = false;
  if (wrap->pending_encrypted_writes.empty()) return;

  PendingEncryptedWrite pending = std::move(wrap->pending_encrypted_writes.front());
  wrap->pending_encrypted_writes.pop_front();
  if (status < 0) {
    EmitError(wrap, CreateErrorWithCode(wrap->env, "ERR_TLS_WRAP", uv_strerror(status)));
  }
  CompleteReq(wrap, &pending.completion_req_ref, status);
  MaybeStartParentShutdown(wrap);
  Cycle(wrap);
}

napi_value InternalWriteOnComplete(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  void* data = nullptr;
  napi_get_cb_info(env, info, &argc, argv, nullptr, &data);
  auto* wrap = static_cast<TlsWrap*>(data);
  int32_t status = 0;
  if (argc >= 1 && argv[0] != nullptr) napi_get_value_int32(env, argv[0], &status);
  OnInternalWriteDone(wrap, status);
  return Undefined(env);
}

napi_value CreateInternalWriteReq(TlsWrap* wrap) {
  napi_value req = UbiCreateStreamReqObject(wrap->env);
  if (req == nullptr) return nullptr;
  napi_value oncomplete = nullptr;
  if (napi_create_function(
          wrap->env, "__ubiTlsInternalWriteDone", NAPI_AUTO_LENGTH, InternalWriteOnComplete, wrap, &oncomplete) !=
          napi_ok ||
      oncomplete == nullptr) {
    return nullptr;
  }
  napi_set_named_property(wrap->env, req, "oncomplete", oncomplete);
  return req;
}

void TryStartParentWrite(TlsWrap* wrap) {
  if (wrap == nullptr || wrap->env == nullptr || wrap->parent_write_in_progress ||
      wrap->has_active_write_issued_by_prev_listener || wrap->pending_encrypted_writes.empty()) {
    return;
  }

  napi_value req = CreateInternalWriteReq(wrap);
  napi_value payload = CreateBufferCopy(
      wrap->env,
      wrap->pending_encrypted_writes.front().data.data(),
      wrap->pending_encrypted_writes.front().data.size());
  if (req == nullptr || payload == nullptr) {
    OnInternalWriteDone(wrap, UV_ENOMEM);
    return;
  }

  napi_value argv[2] = {req, payload};
  napi_value result = nullptr;
  const int32_t rc = CallParentMethodInt(wrap, "writeBuffer", 2, argv, &result);
  if (rc != 0) {
    OnInternalWriteDone(wrap, rc);
    return;
  }
  int32_t* state = UbiGetStreamBaseState(wrap->env);
  const bool async = state != nullptr && state[kUbiLastWriteWasAsync] != 0;
  if (!async) {
    OnInternalWriteDone(wrap, 0);
    return;
  }
  wrap->parent_write_in_progress = true;
}

void MaybeStartParentShutdown(TlsWrap* wrap) {
  if (wrap == nullptr || wrap->pending_shutdown_req_ref == nullptr || wrap->shutdown_started ||
      wrap->parent_write_in_progress || !wrap->pending_encrypted_writes.empty()) {
    return;
  }

  wrap->shutdown_started = true;
  napi_value req_obj = GetRefValue(wrap->env, wrap->pending_shutdown_req_ref);
  if (req_obj == nullptr) {
    DeleteRefIfPresent(wrap->env, &wrap->pending_shutdown_req_ref);
    return;
  }

  napi_value argv[1] = {req_obj};
  const int32_t rc = CallParentMethodInt(wrap, "shutdown", 1, argv, nullptr);
  if (rc != 0) {
    wrap->shutdown_started = false;
    InvokeReqWithStatus(wrap, &wrap->pending_shutdown_req_ref, rc);
    return;
  }

  DeleteRefIfPresent(wrap->env, &wrap->pending_shutdown_req_ref);
}

bool TryHandshake(TlsWrap* wrap) {
  if (wrap == nullptr || wrap->ssl == nullptr) return false;
  if (!wrap->is_server && !wrap->started) return false;
  if (wrap->established) return false;

  const int rc = SSL_do_handshake(wrap->ssl);
  if (rc == 1) {
    wrap->established = true;
  } else if (HandleSslError(wrap, rc, "ERR_TLS_HANDSHAKE", "TLS handshake failed")) {
    return false;
  }

  std::vector<uint8_t> encrypted = ReadAllPendingBio(wrap->enc_out);
  if (!encrypted.empty()) {
    QueueEncryptedWrite(wrap, std::move(encrypted), nullptr);
    return true;
  }
  return rc == 1;
}

bool PumpPendingAppWrites(TlsWrap* wrap) {
  if (wrap == nullptr || wrap->ssl == nullptr || wrap->parent_write_in_progress ||
      wrap->has_active_write_issued_by_prev_listener) {
    return false;
  }
  if (!wrap->is_server && !wrap->started) return false;

  bool made_progress = false;
  while (!wrap->pending_app_writes.empty() && !wrap->parent_write_in_progress &&
         !wrap->has_active_write_issued_by_prev_listener) {
    PendingAppWrite& pending = wrap->pending_app_writes.front();
    const int rc = SSL_write(wrap->ssl, pending.data.data(), static_cast<int>(pending.data.size()));
    if (rc == static_cast<int>(pending.data.size())) {
      napi_ref req_ref = pending.req_ref;
      pending.req_ref = nullptr;
      wrap->pending_app_writes.pop_front();
      std::vector<uint8_t> encrypted = ReadAllPendingBio(wrap->enc_out);
      QueueEncryptedWrite(wrap, std::move(encrypted), req_ref);
      TryStartParentWrite(wrap);
      made_progress = true;
      continue;
    }

    std::vector<uint8_t> encrypted = ReadAllPendingBio(wrap->enc_out);
    if (!encrypted.empty()) {
      QueueEncryptedWrite(wrap, std::move(encrypted), nullptr);
      TryStartParentWrite(wrap);
      made_progress = true;
    }

    if (HandleSslError(wrap, rc, "ERR_TLS_WRITE", "TLS write failed")) {
      napi_ref req_ref = pending.req_ref;
      pending.req_ref = nullptr;
      wrap->pending_app_writes.pop_front();
      CompleteReq(wrap, &req_ref, UV_EPROTO);
      made_progress = true;
      continue;
    }
    break;
  }

  return made_progress;
}

bool ReadCleartext(TlsWrap* wrap) {
  if (wrap == nullptr || wrap->ssl == nullptr) return false;
  bool made_progress = false;
  char buffer[16 * 1024];
  for (;;) {
    const int rc = SSL_read(wrap->ssl, buffer, sizeof(buffer));
    if (rc > 0) {
      EmitOnReadData(wrap, reinterpret_cast<const uint8_t*>(buffer), static_cast<size_t>(rc));
      made_progress = true;
      continue;
    }
    const int err = SSL_get_error(wrap->ssl, rc);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_X509_LOOKUP) break;
    if (err == SSL_ERROR_ZERO_RETURN) {
      if (!wrap->eof) {
        wrap->eof = true;
        EmitOnReadStatus(wrap, UV_EOF);
      }
      break;
    }
    if (err == SSL_ERROR_SSL || err == SSL_ERROR_SYSCALL) {
      EmitError(wrap, CreateLastOpenSslError(wrap->env, "ERR_TLS_READ", "TLS read failed"));
      break;
    }
    break;
  }
  return made_progress;
}

void Cycle(TlsWrap* wrap) {
  if (wrap == nullptr || wrap->ssl == nullptr) return;
  if (wrap->cycle_depth++ > 0) {
    wrap->cycle_depth--;
    return;
  }

  bool keep_going = false;
  do {
    keep_going = false;
    if (TryHandshake(wrap)) keep_going = true;
    if (ReadCleartext(wrap)) keep_going = true;
    if (PumpPendingAppWrites(wrap)) keep_going = true;
    TryStartParentWrite(wrap);
  } while (keep_going && wrap->ssl != nullptr);

  wrap->cycle_depth--;
}

void InitSsl(TlsWrap* wrap) {
  if (wrap == nullptr || wrap->secure_context == nullptr || wrap->secure_context->ctx == nullptr) return;
  wrap->ssl = SSL_new(wrap->secure_context->ctx);
  if (wrap->ssl == nullptr) return;
  wrap->enc_in = BIO_new(BIO_s_mem());
  wrap->enc_out = BIO_new(BIO_s_mem());
  BIO_set_mem_eof_return(wrap->enc_in, -1);
  BIO_set_mem_eof_return(wrap->enc_out, -1);
  SSL_set_bio(wrap->ssl, wrap->enc_in, wrap->enc_out);
  SSL_set_app_data(wrap->ssl, wrap);
  SSL_set_info_callback(wrap->ssl, SslInfoCallback);
  SSL_set_verify(wrap->ssl, SSL_VERIFY_NONE, VerifyCallback);
#ifdef SSL_MODE_RELEASE_BUFFERS
  SSL_set_mode(wrap->ssl, SSL_MODE_RELEASE_BUFFERS);
#endif
  SSL_set_mode(wrap->ssl, SSL_MODE_AUTO_RETRY);
  SSL_set_cert_cb(wrap->ssl, CertCallback, wrap);
  if (wrap->is_server) {
    SSL_set_accept_state(wrap->ssl);
  } else {
    SSL_set_connect_state(wrap->ssl);
  }
}

napi_value ForwardParentRead(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value parent = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &parent, nullptr);
  TlsWrap* wrap = FindWrapByParent(env, parent);
  if (wrap == nullptr || wrap->ssl == nullptr) return Undefined(env);

  int32_t* state = UbiGetStreamBaseState(env);
  const int32_t nread = state != nullptr ? state[kUbiReadBytesOrError] : 0;
  const int32_t offset = state != nullptr ? state[kUbiArrayBufferOffset] : 0;

  if (nread <= 0) {
    ReadCleartext(wrap);
    if (nread < 0) {
      wrap->eof = true;
      EmitOnReadStatus(wrap, nread);
    }
    return Undefined(env);
  }

  const uint8_t* bytes = nullptr;
  size_t len = 0;
  size_t byte_offset = 0;
  if (!GetArrayBufferBytes(env, argv[0], &bytes, &len, &byte_offset) || bytes == nullptr) return Undefined(env);

  size_t final_offset = byte_offset + static_cast<size_t>(offset >= 0 ? offset : 0);
  if (final_offset > len) return Undefined(env);
  size_t final_len = static_cast<size_t>(nread);
  if (final_offset + final_len > len) {
    final_len = len - final_offset;
  }

  if (final_len > 0) {
    (void)BIO_write(wrap->enc_in, bytes + final_offset, static_cast<int>(final_len));
  }
  Cycle(wrap);
  return Undefined(env);
}

napi_value TlsWrapCtor(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  if (self == nullptr) return Undefined(env);

  auto* wrap = new TlsWrap();
  wrap->env = env;
  wrap->async_id = EnsureState(env).next_async_id++;
  if (napi_wrap(env, self, wrap, TlsWrapFinalize, nullptr, &wrap->wrapper_ref) != napi_ok) {
    delete wrap;
    return Undefined(env);
  }

  EnsureState(env).wraps.push_back(wrap);
  napi_set_named_property(env, self, "isStreamBase", MakeBool(env, true));
  napi_set_named_property(env, self, "reading", MakeBool(env, false));
  return self;
}

napi_value TlsWrapReadStart(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, &self);
  if (wrap == nullptr) return MakeInt32(env, UV_EINVAL);
  const int32_t rc = CallParentMethodInt(wrap, "readStart", 0, nullptr, nullptr);
  if (self != nullptr) napi_set_named_property(env, self, "reading", MakeBool(env, rc == 0));
  return MakeInt32(env, rc);
}

napi_value TlsWrapReadStop(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, &self);
  if (wrap == nullptr) return MakeInt32(env, UV_EINVAL);
  const int32_t rc = CallParentMethodInt(wrap, "readStop", 0, nullptr, nullptr);
  if (self != nullptr) napi_set_named_property(env, self, "reading", MakeBool(env, false));
  return MakeInt32(env, rc);
}

int32_t QueueAppWrite(TlsWrap* wrap, napi_value req_obj, const uint8_t* data, size_t len) {
  if (wrap == nullptr || wrap->ssl == nullptr) return UV_EINVAL;
  PendingAppWrite pending;
  if (req_obj != nullptr) napi_create_reference(wrap->env, req_obj, 1, &pending.req_ref);
  pending.data.assign(data, data + len);
  wrap->pending_app_writes.push_back(std::move(pending));
  SetState(wrap->env, kUbiBytesWritten, static_cast<int32_t>(len));
  SetState(wrap->env, kUbiLastWriteWasAsync, 1);
  Cycle(wrap);
  return 0;
}

napi_value TlsWrapWriteBuffer(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || argc < 2) return MakeInt32(env, UV_EINVAL);
  const uint8_t* data = nullptr;
  size_t len = 0;
  bool refable = false;
  std::string temp_utf8;
  if (!UbiStreamBaseExtractByteSpan(env, argv[1], &data, &len, &refable, &temp_utf8) || data == nullptr) {
    return MakeInt32(env, UV_EINVAL);
  }
  return MakeInt32(env, QueueAppWrite(wrap, argv[0], data, len));
}

napi_value TlsWrapWriteString(napi_env env, napi_callback_info info, const char* encoding_name) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || argc < 2) return MakeInt32(env, UV_EINVAL);
  napi_value payload = argv[1];
  if (encoding_name != nullptr) {
    napi_value encoding = nullptr;
    if (napi_create_string_utf8(env, encoding_name, NAPI_AUTO_LENGTH, &encoding) == napi_ok && encoding != nullptr) {
      payload = UbiStreamBufferFromWithEncoding(env, argv[1], encoding);
    }
  }
  const uint8_t* data = nullptr;
  size_t len = 0;
  bool refable = false;
  std::string temp_utf8;
  if (!UbiStreamBaseExtractByteSpan(env, payload, &data, &len, &refable, &temp_utf8) || data == nullptr) {
    return MakeInt32(env, UV_EINVAL);
  }
  return MakeInt32(env, QueueAppWrite(wrap, argv[0], data, len));
}

napi_value TlsWrapWriteLatin1String(napi_env env, napi_callback_info info) {
  return TlsWrapWriteString(env, info, "latin1");
}

napi_value TlsWrapWriteUtf8String(napi_env env, napi_callback_info info) {
  return TlsWrapWriteString(env, info, "utf8");
}

napi_value TlsWrapWriteAsciiString(napi_env env, napi_callback_info info) {
  return TlsWrapWriteString(env, info, "ascii");
}

napi_value TlsWrapWriteUcs2String(napi_env env, napi_callback_info info) {
  return TlsWrapWriteString(env, info, "ucs2");
}

napi_value TlsWrapWritev(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || argc < 2) return MakeInt32(env, UV_EINVAL);
  bool all_buffers = false;
  if (argc >= 3 && argv[2] != nullptr) napi_get_value_bool(env, argv[2], &all_buffers);
  uint32_t raw_len = 0;
  if (napi_get_array_length(env, argv[1], &raw_len) != napi_ok) return MakeInt32(env, UV_EINVAL);
  const uint32_t count = all_buffers ? raw_len : (raw_len / 2);
  std::vector<uint8_t> combined;
  for (uint32_t i = 0; i < count; ++i) {
    napi_value chunk = nullptr;
    napi_get_element(env, argv[1], all_buffers ? i : (i * 2), &chunk);
    const uint8_t* data = nullptr;
    size_t len = 0;
    bool refable = false;
    std::string temp_utf8;
    if (!UbiStreamBaseExtractByteSpan(env, chunk, &data, &len, &refable, &temp_utf8) || data == nullptr) {
      return MakeInt32(env, UV_EINVAL);
    }
    combined.insert(combined.end(), data, data + len);
  }
  return MakeInt32(env, QueueAppWrite(wrap, argv[0], combined.data(), combined.size()));
}

napi_value TlsWrapShutdown(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || argc < 1) return MakeInt32(env, UV_EINVAL);
  if (wrap->shutdown_started || wrap->pending_shutdown_req_ref != nullptr) {
    return MakeInt32(env, 0);
  }
  if (wrap->ssl == nullptr) {
    wrap->shutdown_started = true;
    const int32_t rc = CallParentMethodInt(wrap, "shutdown", 1, argv, nullptr);
    if (rc != 0) wrap->shutdown_started = false;
    return MakeInt32(env, rc);
  }

  int shutdown_rc = SSL_shutdown(wrap->ssl);
  if (shutdown_rc == 0) {
    shutdown_rc = SSL_shutdown(wrap->ssl);
  }
  std::vector<uint8_t> encrypted = ReadAllPendingBio(wrap->enc_out);
  if (!encrypted.empty()) {
    QueueEncryptedWrite(wrap, std::move(encrypted), nullptr);
  }
  TryStartParentWrite(wrap);

  if (wrap->parent_write_in_progress || !wrap->pending_encrypted_writes.empty()) {
    if (napi_create_reference(env, argv[0], 1, &wrap->pending_shutdown_req_ref) != napi_ok) {
      return MakeInt32(env, UV_ENOMEM);
    }
    return MakeInt32(env, 0);
  }

  wrap->shutdown_started = true;
  const int32_t rc = CallParentMethodInt(wrap, "shutdown", 1, argv, nullptr);
  if (rc != 0) wrap->shutdown_started = false;
  return MakeInt32(env, rc);
}

napi_value TlsWrapClose(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr) return Undefined(env);
  DestroySsl(wrap);
  (void)CallParentMethodInt(wrap, "close", argc, argv, nullptr);
  return Undefined(env);
}

napi_value TlsWrapRef(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap != nullptr) (void)CallParentMethodInt(wrap, "ref", 0, nullptr, nullptr);
  return Undefined(env);
}

napi_value TlsWrapUnref(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap != nullptr) (void)CallParentMethodInt(wrap, "unref", 0, nullptr, nullptr);
  return Undefined(env);
}

napi_value TlsWrapGetAsyncId(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  return MakeInt64(env, wrap != nullptr ? wrap->async_id : -1);
}

napi_value TlsWrapGetProviderType(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  napi_value result = nullptr;
  int32_t rc = wrap == nullptr ? UV_EINVAL : CallParentMethodInt(wrap, "getProviderType", 0, nullptr, &result);
  if (wrap == nullptr || rc == UV_EINVAL || result == nullptr) return MakeInt32(env, 0);
  return result;
}

napi_value TlsWrapAsyncReset(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap != nullptr) {
    wrap->async_id = EnsureState(env).next_async_id++;
  }
  return Undefined(env);
}

napi_value TlsWrapUseUserBuffer(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap != nullptr && argc >= 1) {
    (void)CallParentMethodInt(wrap, "useUserBuffer", 1, argv, nullptr);
  }
  return Undefined(env);
}

napi_value TlsWrapBytesReadGetter(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  TlsWrap* wrap = UnwrapTlsWrap(env, self);
  napi_value parent = wrap != nullptr ? GetRefValue(env, wrap->parent_ref) : nullptr;
  napi_value out = GetNamedValue(env, parent, "bytesRead");
  return out != nullptr ? out : MakeInt32(env, 0);
}

napi_value TlsWrapBytesWrittenGetter(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  TlsWrap* wrap = UnwrapTlsWrap(env, self);
  napi_value parent = wrap != nullptr ? GetRefValue(env, wrap->parent_ref) : nullptr;
  napi_value out = GetNamedValue(env, parent, "bytesWritten");
  return out != nullptr ? out : MakeInt32(env, 0);
}

napi_value TlsWrapFdGetter(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  TlsWrap* wrap = UnwrapTlsWrap(env, self);
  napi_value parent = wrap != nullptr ? GetRefValue(env, wrap->parent_ref) : nullptr;
  napi_value out = GetNamedValue(env, parent, "fd");
  return out != nullptr ? out : MakeInt32(env, -1);
}

napi_value TlsWrapSetVerifyMode(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr || argc < 2) return Undefined(env);
  bool request_cert = false;
  bool reject_unauthorized = false;
  napi_get_value_bool(env, argv[0], &request_cert);
  napi_get_value_bool(env, argv[1], &reject_unauthorized);
  wrap->request_cert = request_cert;
  wrap->reject_unauthorized = reject_unauthorized;

  int verify_mode = SSL_VERIFY_NONE;
  if (wrap->is_server) {
    if (request_cert) {
      verify_mode = SSL_VERIFY_PEER;
      if (reject_unauthorized) verify_mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
    }
  }
  SSL_set_verify(wrap->ssl, verify_mode, VerifyCallback);
  return Undefined(env);
}

napi_value TlsWrapEnableTrace(napi_env env, napi_callback_info info) {
  (void)env;
  (void)info;
  return Undefined(env);
}

napi_value TlsWrapEnableSessionCallbacks(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr || wrap->session_callbacks_enabled) return Undefined(env);
  SSL_CTX* ctx = SSL_get_SSL_CTX(wrap->ssl);
  if (ctx == nullptr) return Undefined(env);
  wrap->session_callbacks_enabled = true;
  SSL_CTX_sess_set_new_cb(ctx, NewSessionCallback);
  if (wrap->is_server) {
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER);
  } else {
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_CLIENT);
  }
  return Undefined(env);
}

napi_value TlsWrapEnableCertCb(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap != nullptr) wrap->waiting_cert_cb = true;
  return Undefined(env);
}

napi_value TlsWrapEnableALPNCb(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap != nullptr && wrap->ssl != nullptr) {
    wrap->alpn_callback_enabled = true;
    SSL_CTX_set_alpn_select_cb(SSL_get_SSL_CTX(wrap->ssl), SelectALPNCallback, nullptr);
  }
  return Undefined(env);
}

napi_value TlsWrapEnablePskCallback(napi_env env, napi_callback_info info) {
  (void)env;
  (void)info;
  return Undefined(env);
}

napi_value TlsWrapSetPskIdentityHint(napi_env env, napi_callback_info info) {
  (void)env;
  (void)info;
  return Undefined(env);
}

napi_value TlsWrapEnableKeylogCallback(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap != nullptr && wrap->ssl != nullptr) {
    SSL_CTX_set_keylog_callback(SSL_get_SSL_CTX(wrap->ssl), KeylogCallback);
  }
  return Undefined(env);
}

napi_value TlsWrapWritesIssuedByPrevListenerDone(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap != nullptr) {
    wrap->has_active_write_issued_by_prev_listener = false;
    TryStartParentWrite(wrap);
    Cycle(wrap);
  }
  return Undefined(env);
}

napi_value TlsWrapSetALPNProtocols(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr || argc < 1) return Undefined(env);
  const uint8_t* data = nullptr;
  size_t len = 0;
  bool refable = false;
  std::string temp_utf8;
  if (!UbiStreamBaseExtractByteSpan(env, argv[0], &data, &len, &refable, &temp_utf8) || data == nullptr) {
    napi_throw_type_error(env, nullptr, "Must give a Buffer as first argument");
    return nullptr;
  }
  if (wrap->is_server) {
    wrap->alpn_protos.assign(data, data + len);
    SSL_CTX_set_alpn_select_cb(SSL_get_SSL_CTX(wrap->ssl), SelectALPNCallback, nullptr);
  } else {
    (void)SSL_set_alpn_protos(wrap->ssl, data, static_cast<unsigned int>(len));
  }
  return Undefined(env);
}

napi_value TlsWrapRequestOCSP(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap != nullptr && wrap->ssl != nullptr && !wrap->is_server) {
    SSL_set_tlsext_status_type(wrap->ssl, TLSEXT_STATUSTYPE_ocsp);
  }
  return Undefined(env);
}

napi_value TlsWrapStart(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr) return Undefined(env);
  wrap->started = true;
  Cycle(wrap);
  return Undefined(env);
}

napi_value TlsWrapRenegotiate(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr) return Undefined(env);
#ifndef OPENSSL_IS_BORINGSSL
  (void)SSL_renegotiate(wrap->ssl);
#endif
  Cycle(wrap);
  return Undefined(env);
}

napi_value TlsWrapSetServername(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr || argc < 1) return Undefined(env);
  const std::string servername = ValueToUtf8(env, argv[0]);
  if (!servername.empty()) {
    (void)SSL_set_tlsext_host_name(wrap->ssl, servername.c_str());
  }
  return Undefined(env);
}

napi_value TlsWrapGetServername(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr) return MakeBool(env, false);
  const char* servername = SSL_get_servername(wrap->ssl, TLSEXT_NAMETYPE_host_name);
  if (servername == nullptr) return MakeBool(env, false);
  napi_value out = nullptr;
  napi_create_string_utf8(env, servername, NAPI_AUTO_LENGTH, &out);
  return out != nullptr ? out : MakeBool(env, false);
}

bool LoadSessionBytes(TlsWrap* wrap, const uint8_t* data, size_t len) {
  if (wrap == nullptr || wrap->ssl == nullptr || data == nullptr || len == 0) return false;
  const unsigned char* ptr = data;
  SSL_SESSION* session = d2i_SSL_SESSION(nullptr, &ptr, static_cast<long>(len));
  if (session == nullptr) return false;
  const int rc = SSL_set_session(wrap->ssl, session);
  SSL_SESSION_free(session);
  return rc == 1;
}

napi_value TlsWrapSetSession(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || argc < 1) return Undefined(env);
  const uint8_t* data = nullptr;
  size_t len = 0;
  bool refable = false;
  std::string temp_utf8;
  if (UbiStreamBaseExtractByteSpan(env, argv[0], &data, &len, &refable, &temp_utf8) && data != nullptr) {
    wrap->pending_session.assign(data, data + len);
    if (wrap->ssl != nullptr) {
      (void)LoadSessionBytes(wrap, wrap->pending_session.data(), wrap->pending_session.size());
    }
  }
  return Undefined(env);
}

napi_value TlsWrapLoadSession(napi_env env, napi_callback_info info) {
  return TlsWrapSetSession(env, info);
}

napi_value TlsWrapGetSession(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr) return Undefined(env);
  SSL_SESSION* session = SSL_get_session(wrap->ssl);
  if (session == nullptr) return Undefined(env);
  const int size = i2d_SSL_SESSION(session, nullptr);
  if (size <= 0) return Undefined(env);
  std::vector<uint8_t> out(static_cast<size_t>(size));
  unsigned char* ptr = out.data();
  if (i2d_SSL_SESSION(session, &ptr) != size) return Undefined(env);
  napi_value buffer = CreateBufferCopy(env, out.data(), out.size());
  return buffer != nullptr ? buffer : Undefined(env);
}

napi_value TlsWrapExportKeyingMaterial(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr || argc < 2) return Undefined(env);
  uint32_t length = 0;
  napi_get_value_uint32(env, argv[0], &length);
  const std::string label = ValueToUtf8(env, argv[1]);
  std::vector<uint8_t> context_bytes;
  const uint8_t* context_data = nullptr;
  size_t context_len = 0;
  if (argc >= 3 && argv[2] != nullptr) {
    bool refable = false;
    std::string temp_utf8;
    if (UbiStreamBaseExtractByteSpan(env, argv[2], &context_data, &context_len, &refable, &temp_utf8) &&
        context_data != nullptr) {
      context_bytes.assign(context_data, context_data + context_len);
      context_data = context_bytes.data();
    }
  }
  std::vector<uint8_t> out(length, 0);
  if (SSL_export_keying_material(wrap->ssl,
                                 out.data(),
                                 out.size(),
                                 label.c_str(),
                                 label.size(),
                                 context_bytes.empty() ? nullptr : context_data,
                                 context_len,
                                 context_bytes.empty() ? 0 : 1) != 1) {
    EmitError(wrap, CreateLastOpenSslError(env, "ERR_TLS_EXPORT_KEYING_MATERIAL", "Key export failed"));
    return Undefined(env);
  }
  napi_value buffer = CreateBufferCopy(env, out.data(), out.size());
  return buffer != nullptr ? buffer : Undefined(env);
}

napi_value TlsWrapSetMaxSendFragment(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr || argc < 1) return MakeInt32(env, 0);
#ifdef SSL_set_max_send_fragment
  uint32_t value = 0;
  napi_get_value_uint32(env, argv[0], &value);
  return MakeInt32(env, SSL_set_max_send_fragment(wrap->ssl, value) == 1 ? 1 : 0);
#else
  return MakeInt32(env, 1);
#endif
}

napi_value TlsWrapGetALPNNegotiatedProtocol(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr) return MakeBool(env, false);
  const unsigned char* data = nullptr;
  unsigned int len = 0;
  SSL_get0_alpn_selected(wrap->ssl, &data, &len);
  if (data == nullptr || len == 0) return MakeBool(env, false);
  napi_value out = nullptr;
  napi_create_string_utf8(env, reinterpret_cast<const char*>(data), len, &out);
  return out != nullptr ? out : MakeBool(env, false);
}

napi_value TlsWrapGetCipher(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr) return Undefined(env);
  const SSL_CIPHER* cipher = SSL_get_current_cipher(wrap->ssl);
  if (cipher == nullptr) return Undefined(env);
  napi_value out = nullptr;
  napi_create_object(env, &out);
  napi_value name = nullptr;
  napi_value standard_name = nullptr;
  napi_value version = nullptr;
  const char* cipher_name = SSL_CIPHER_get_name(cipher);
  const char* cipher_standard_name = SSL_CIPHER_standard_name(cipher);
  const char* cipher_version = SSL_CIPHER_get_version(cipher);
  if (cipher_name != nullptr) napi_create_string_utf8(env, cipher_name, NAPI_AUTO_LENGTH, &name);
  if (cipher_standard_name != nullptr) {
    napi_create_string_utf8(env, cipher_standard_name, NAPI_AUTO_LENGTH, &standard_name);
  } else if (cipher_name != nullptr) {
    napi_create_string_utf8(env, cipher_name, NAPI_AUTO_LENGTH, &standard_name);
  }
  if (cipher_version != nullptr) napi_create_string_utf8(env, cipher_version, NAPI_AUTO_LENGTH, &version);
  if (name != nullptr) napi_set_named_property(env, out, "name", name);
  if (standard_name != nullptr) napi_set_named_property(env, out, "standardName", standard_name);
  if (version != nullptr) napi_set_named_property(env, out, "version", version);
  return out != nullptr ? out : Undefined(env);
}

napi_value TlsWrapGetSharedSigalgs(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  napi_value out = nullptr;
  napi_create_array(env, &out);
  if (wrap == nullptr || wrap->ssl == nullptr) return out != nullptr ? out : Undefined(env);
  int nsig = SSL_get_shared_sigalgs(wrap->ssl, 0, nullptr, nullptr, nullptr, nullptr, nullptr);
  for (int i = 0; i < nsig; ++i) {
    int sign_nid = NID_undef;
    const char* sign_name = nullptr;
    if (SSL_get_shared_sigalgs(wrap->ssl, i, &sign_nid, nullptr, nullptr, nullptr, nullptr) > 0) {
      sign_name = OBJ_nid2sn(sign_nid);
    }
    if (sign_name != nullptr) {
      napi_value value = nullptr;
      napi_create_string_utf8(env, sign_name, NAPI_AUTO_LENGTH, &value);
      napi_set_element(env, out, i, value);
    }
  }
  return out != nullptr ? out : Undefined(env);
}

napi_value TlsWrapGetEphemeralKeyInfo(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  napi_value out = nullptr;
  napi_create_object(env, &out);
  if (wrap == nullptr || wrap->ssl == nullptr || wrap->is_server) return out != nullptr ? out : Undefined(env);
  EVP_PKEY* key = nullptr;
  if (SSL_get_peer_tmp_key(wrap->ssl, &key) != 1 || key == nullptr) return out != nullptr ? out : Undefined(env);
  const int key_id = EVP_PKEY_base_id(key);
  const int bits = EVP_PKEY_bits(key);
  const char* key_type = nullptr;
  if (key_id == EVP_PKEY_DH) {
    key_type = "DH";
  } else if (key_id == EVP_PKEY_EC) {
    key_type = "ECDH";
  }
  if (key_type != nullptr) {
    napi_value type = nullptr;
    napi_create_string_utf8(env, key_type, NAPI_AUTO_LENGTH, &type);
    if (type != nullptr) napi_set_named_property(env, out, "type", type);
  }
  if (bits > 0) {
    napi_set_named_property(env, out, "size", MakeInt32(env, bits));
  }
  EVP_PKEY_free(key);
  return out != nullptr ? out : Undefined(env);
}

napi_value TlsWrapGetFinished(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr) return Undefined(env);
  char dummy[1];
  const size_t len = SSL_get_finished(wrap->ssl, dummy, sizeof(dummy));
  if (len == 0) return Undefined(env);
  std::vector<uint8_t> out(len);
  (void)SSL_get_finished(wrap->ssl, out.data(), out.size());
  napi_value buffer = CreateBufferCopy(env, out.data(), out.size());
  return buffer != nullptr ? buffer : Undefined(env);
}

napi_value TlsWrapGetPeerFinished(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr) return Undefined(env);
  char dummy[1];
  const size_t len = SSL_get_peer_finished(wrap->ssl, dummy, sizeof(dummy));
  if (len == 0) return Undefined(env);
  std::vector<uint8_t> out(len);
  (void)SSL_get_peer_finished(wrap->ssl, out.data(), out.size());
  napi_value buffer = CreateBufferCopy(env, out.data(), out.size());
  return buffer != nullptr ? buffer : Undefined(env);
}

napi_value TlsWrapGetProtocol(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr) return Undefined(env);
  const char* version = SSL_get_version(wrap->ssl);
  if (version == nullptr) return Undefined(env);
  napi_value out = nullptr;
  napi_create_string_utf8(env, version, NAPI_AUTO_LENGTH, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value TlsWrapGetTLSTicket(napi_env env, napi_callback_info info) {
  (void)env;
  (void)info;
  return Undefined(env);
}

napi_value TlsWrapIsSessionReused(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  return MakeBool(env, wrap != nullptr && wrap->ssl != nullptr && SSL_session_reused(wrap->ssl) == 1);
}

bool AppendX509NameEntry(napi_env env, napi_value target, int nid, const std::string& value) {
  const char* key = OBJ_nid2sn(nid);
  if (key == nullptr) return true;
  napi_value current = GetNamedValue(env, target, key);
  napi_value str = nullptr;
  if (napi_create_string_utf8(env, value.c_str(), value.size(), &str) != napi_ok || str == nullptr) return false;
  if (current == nullptr || current == Undefined(env) || current == Null(env)) {
    return napi_set_named_property(env, target, key, str) == napi_ok;
  }
  bool is_array = false;
  if (napi_is_array(env, current, &is_array) == napi_ok && is_array) {
    uint32_t length = 0;
    napi_get_array_length(env, current, &length);
    napi_set_element(env, current, length, str);
    return true;
  }
  napi_value arr = nullptr;
  if (napi_create_array_with_length(env, 2, &arr) != napi_ok || arr == nullptr) return false;
  napi_set_element(env, arr, 0, current);
  napi_set_element(env, arr, 1, str);
  return napi_set_named_property(env, target, key, arr) == napi_ok;
}

napi_value CreateX509NameObject(napi_env env, X509_NAME* name) {
  napi_value out = nullptr;
  napi_create_object(env, &out);
  if (out == nullptr || name == nullptr) return out;
  const int count = X509_NAME_entry_count(name);
  for (int i = 0; i < count; ++i) {
    X509_NAME_ENTRY* entry = X509_NAME_get_entry(name, i);
    if (entry == nullptr) continue;
    ASN1_OBJECT* object = X509_NAME_ENTRY_get_object(entry);
    ASN1_STRING* data = X509_NAME_ENTRY_get_data(entry);
    unsigned char* utf8 = nullptr;
    const int utf8_len = ASN1_STRING_to_UTF8(&utf8, data);
    if (utf8_len < 0 || utf8 == nullptr) continue;
    std::string value(reinterpret_cast<char*>(utf8), static_cast<size_t>(utf8_len));
    OPENSSL_free(utf8);
    (void)AppendX509NameEntry(env, out, OBJ_obj2nid(object), value);
  }
  return out;
}

std::string GetSubjectAltNameString(X509* cert) {
  std::string out;
  if (cert == nullptr) return out;
  GENERAL_NAMES* names =
      static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
  if (names == nullptr) return out;
  const int count = sk_GENERAL_NAME_num(names);
  for (int i = 0; i < count; ++i) {
    const GENERAL_NAME* name = sk_GENERAL_NAME_value(names, i);
    if (name == nullptr) continue;
    if (!out.empty()) out.append(", ");
    if (name->type == GEN_DNS) {
      const auto* dns = ASN1_STRING_get0_data(name->d.dNSName);
      const int dns_len = ASN1_STRING_length(name->d.dNSName);
      out.append("DNS:");
      out.append(reinterpret_cast<const char*>(dns), static_cast<size_t>(dns_len));
    } else if (name->type == GEN_IPADD) {
      out.append("IP Address:");
      const unsigned char* ip = ASN1_STRING_get0_data(name->d.iPAddress);
      const int ip_len = ASN1_STRING_length(name->d.iPAddress);
      char buf[INET6_ADDRSTRLEN] = {0};
      if (ip_len == 4) {
        uv_inet_ntop(AF_INET, ip, buf, sizeof(buf));
      } else if (ip_len == 16) {
        uv_inet_ntop(AF_INET6, ip, buf, sizeof(buf));
      }
      out.append(buf);
    } else {
      out.pop_back();
      out.pop_back();
    }
  }
  GENERAL_NAMES_free(names);
  return out;
}

napi_value CreateLegacyCertObject(napi_env env, X509* cert) {
  if (cert == nullptr) return Undefined(env);
  napi_value out = nullptr;
  napi_create_object(env, &out);
  if (out == nullptr) return Undefined(env);

  napi_value subject = CreateX509NameObject(env, X509_get_subject_name(cert));
  if (subject != nullptr) napi_set_named_property(env, out, "subject", subject);

  napi_value issuer = CreateX509NameObject(env, X509_get_issuer_name(cert));
  if (issuer != nullptr) napi_set_named_property(env, out, "issuer", issuer);

  const std::string san = GetSubjectAltNameString(cert);
  if (!san.empty()) {
    napi_value san_v = nullptr;
    napi_create_string_utf8(env, san.c_str(), san.size(), &san_v);
    if (san_v != nullptr) napi_set_named_property(env, out, "subjectaltname", san_v);
  }

  napi_value ca = MakeBool(env, X509_check_ca(cert) == 1);
  if (ca != nullptr) napi_set_named_property(env, out, "ca", ca);

  const int der_len = i2d_X509(cert, nullptr);
  if (der_len > 0) {
    std::vector<uint8_t> der(static_cast<size_t>(der_len));
    unsigned char* ptr = der.data();
    if (i2d_X509(cert, &ptr) == der_len) {
      napi_value raw = CreateBufferCopy(env, der.data(), der.size());
      if (raw != nullptr) napi_set_named_property(env, out, "raw", raw);
    }
  }

  return out;
}

napi_value TlsWrapVerifyError(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr) return Null(env);
  const long verify_error = SSL_get_verify_result(wrap->ssl);
  if (verify_error == X509_V_OK) return Null(env);
  const char* code = ncrypto::X509Pointer::ErrorCode(static_cast<int32_t>(verify_error));
  const char* reason = X509_verify_cert_error_string(verify_error);
  return CreateErrorWithCode(env, code != nullptr ? code : "ERR_TLS_CERT", reason != nullptr ? reason
                                                                                              : "Certificate verification failed");
}

napi_value TlsWrapGetPeerCertificate(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr) return Undefined(env);
  X509* cert = SSL_get_peer_certificate(wrap->ssl);
  if (cert == nullptr) return Undefined(env);
  napi_value out = CreateLegacyCertObject(env, cert);
  X509_free(cert);
  return out != nullptr ? out : Undefined(env);
}

napi_value TlsWrapGetCertificate(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr) return Undefined(env);
  X509* cert = SSL_get_certificate(wrap->ssl);
  return CreateLegacyCertObject(env, cert);
}

napi_value TlsWrapGetPeerX509Certificate(napi_env env, napi_callback_info info) {
  (void)env;
  (void)info;
  return Undefined(env);
}

napi_value TlsWrapGetX509Certificate(napi_env env, napi_callback_info info) {
  (void)env;
  (void)info;
  return Undefined(env);
}

napi_value TlsWrapSetKeyCert(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr || wrap->is_server == false || argc < 1) return Undefined(env);
  ubi::crypto::SecureContextHolder* holder = nullptr;
  if (internal_binding::UbiCryptoGetSecureContextHolderFromObject(env, argv[0], &holder) && holder != nullptr) {
    if (!SetSecureContextOnSsl(wrap, holder)) {
      EmitError(wrap, CreateLastOpenSslError(env, "ERR_TLS_INVALID_CONTEXT", "Failed to update secure context"));
    }
  } else {
    napi_throw_type_error(env, nullptr, "Must give a SecureContext as first argument");
    return nullptr;
  }
  return Undefined(env);
}

napi_value TlsWrapDestroySSL(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  DestroySsl(wrap);
  return Undefined(env);
}

napi_value TlsWrapEndParser(napi_env env, napi_callback_info info) {
  (void)env;
  (void)info;
  return Undefined(env);
}

napi_value TlsWrapSetOCSPResponse(napi_env env, napi_callback_info info) {
  (void)env;
  (void)info;
  return Undefined(env);
}

napi_value TlsWrapCertCbDone(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr) return Undefined(env);
  napi_value self = GetRefValue(env, wrap->wrapper_ref);
  napi_value sni_context = GetNamedValue(env, self, "sni_context");
  ubi::crypto::SecureContextHolder* holder = nullptr;
  if (sni_context != nullptr &&
      internal_binding::UbiCryptoGetSecureContextHolderFromObject(env, sni_context, &holder) &&
      holder != nullptr) {
    if (!SetSecureContextOnSsl(wrap, holder)) {
      EmitError(wrap, CreateLastOpenSslError(env, "ERR_TLS_INVALID_CONTEXT", "Failed to set SNI context"));
    }
  }
  wrap->cert_cb_running = false;
  wrap->waiting_cert_cb = false;
  Cycle(wrap);
  return Undefined(env);
}

napi_value TlsWrapNewSessionDone(napi_env env, napi_callback_info info) {
  (void)env;
  (void)info;
  return Undefined(env);
}

napi_value TlsWrapReceive(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr || argc < 1) return Undefined(env);
  const uint8_t* data = nullptr;
  size_t len = 0;
  size_t offset = 0;
  if (!GetArrayBufferBytes(env, argv[0], &data, &len, &offset) || data == nullptr) return Undefined(env);
  if (len > offset) {
    (void)BIO_write(wrap->enc_in, data + offset, static_cast<int>(len - offset));
  }
  Cycle(wrap);
  return Undefined(env);
}

napi_value TlsWrapWrap(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 2 || argv[0] == nullptr || argv[1] == nullptr) return Undefined(env);

  ubi::crypto::SecureContextHolder* holder = nullptr;
  if (!internal_binding::UbiCryptoGetSecureContextHolderFromObject(env, argv[1], &holder) || holder == nullptr) {
    napi_throw_type_error(env, nullptr, "SecureContext required");
    return nullptr;
  }

  napi_value ctor = GetRefValue(env, EnsureState(env).tls_wrap_ctor_ref);
  napi_value out = nullptr;
  if (ctor == nullptr || napi_new_instance(env, ctor, 0, nullptr, &out) != napi_ok || out == nullptr) {
    return Undefined(env);
  }

  TlsWrap* wrap = UnwrapTlsWrap(env, out);
  if (wrap == nullptr) return Undefined(env);
  napi_create_reference(env, argv[0], 1, &wrap->parent_ref);
  napi_create_reference(env, argv[1], 1, &wrap->context_ref);
  wrap->secure_context = holder;
  if (argc >= 3 && argv[2] != nullptr) {
    bool is_server = false;
    napi_get_value_bool(env, argv[2], &is_server);
    wrap->is_server = is_server;
  }
  if (argc >= 4 && argv[3] != nullptr) {
    bool has_active = false;
    napi_get_value_bool(env, argv[3], &has_active);
    wrap->has_active_write_issued_by_prev_listener = has_active;
  }

  InitSsl(wrap);
  if (!wrap->pending_session.empty()) {
    (void)LoadSessionBytes(wrap, wrap->pending_session.data(), wrap->pending_session.size());
  }

  napi_value parent_reading = GetNamedValue(env, argv[0], "reading");
  if (parent_reading != nullptr) {
    napi_set_named_property(env, out, "reading", parent_reading);
  }

  napi_value onread = nullptr;
  if (napi_create_function(env, "__ubiTlsParentOnRead", NAPI_AUTO_LENGTH, ForwardParentRead, nullptr, &onread) ==
          napi_ok &&
      onread != nullptr) {
    napi_set_named_property(env, argv[0], "onread", onread);
  }

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
      {"writeLatin1String", nullptr, TlsWrapWriteLatin1String, nullptr, nullptr, nullptr, napi_default_method,
       nullptr},
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
      {"enablePskCallback", nullptr, TlsWrapEnablePskCallback, nullptr, nullptr, nullptr, napi_default_method,
       nullptr},
      {"setPskIdentityHint", nullptr, TlsWrapSetPskIdentityHint, nullptr, nullptr, nullptr, napi_default_method,
       nullptr},
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
      {"getPeerX509Certificate", nullptr, TlsWrapGetPeerX509Certificate, nullptr, nullptr, nullptr,
       napi_default_method, nullptr},
      {"getX509Certificate", nullptr, TlsWrapGetX509Certificate, nullptr, nullptr, nullptr, napi_default_method,
       nullptr},
      {"exportKeyingMaterial", nullptr, TlsWrapExportKeyingMaterial, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"setMaxSendFragment", nullptr, TlsWrapSetMaxSendFragment, nullptr, nullptr, nullptr, napi_default_method,
       nullptr},
      {"getALPNNegotiatedProtocol", nullptr, TlsWrapGetALPNNegotiatedProtocol, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"verifyError", nullptr, TlsWrapVerifyError, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getPeerCertificate", nullptr, TlsWrapGetPeerCertificate, nullptr, nullptr, nullptr, napi_default_method,
       nullptr},
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

  DeleteRefIfPresent(env, &state.tls_wrap_ctor_ref);
  napi_create_reference(env, tls_wrap_ctor, 1, &state.tls_wrap_ctor_ref);

  napi_value wrap_fn = nullptr;
  if (napi_create_function(env, "wrap", NAPI_AUTO_LENGTH, TlsWrapWrap, nullptr, &wrap_fn) != napi_ok ||
      wrap_fn == nullptr) {
    return nullptr;
  }

  napi_set_named_property(env, binding, "TLSWrap", tls_wrap_ctor);
  napi_set_named_property(env, binding, "wrap", wrap_fn);

  DeleteRefIfPresent(env, &state.binding_ref);
  napi_create_reference(env, binding, 1, &state.binding_ref);
  return binding;
}

}  // namespace

napi_value UbiInstallTlsWrapBinding(napi_env env) {
  if (env == nullptr) return nullptr;
  return UbiInstallTlsWrapBindingInternal(env);
}
