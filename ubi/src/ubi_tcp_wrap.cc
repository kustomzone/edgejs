#include "ubi_tcp_wrap.h"

#include <arpa/inet.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <uv.h>

#include "ubi_async_wrap.h"
#include "ubi_env_loop.h"
#include "ubi_runtime.h"
#include "ubi_stream_base.h"
#include "ubi_stream_listener.h"

namespace {

constexpr int kTcpSocket = 0;
constexpr int kTcpServer = 1;

struct TcpWrap;

struct ConnectReqWrap {
  uv_connect_t req{};
  napi_env env = nullptr;
  napi_ref req_obj_ref = nullptr;
  TcpWrap* tcp = nullptr;
};

struct TcpWrap {
  napi_env env = nullptr;
  UbiStreamBase base{};
  uv_tcp_t handle{};
  int socket_type = kTcpSocket;
};

struct TcpBindingState {
  napi_ref tcp_ctor_ref = nullptr;
  napi_ref connect_wrap_ctor_ref = nullptr;
  napi_ref tcp_binding_ref = nullptr;
};

std::unordered_map<napi_env, TcpBindingState> g_tcp_states;
std::unordered_set<napi_env> g_tcp_cleanup_hook_registered;

void OnTcpEnvCleanup(void* data) {
  napi_env env = static_cast<napi_env>(data);
  g_tcp_cleanup_hook_registered.erase(env);

  auto it = g_tcp_states.find(env);
  if (it == g_tcp_states.end()) return;
  if (it->second.tcp_ctor_ref != nullptr) napi_delete_reference(env, it->second.tcp_ctor_ref);
  if (it->second.connect_wrap_ctor_ref != nullptr) napi_delete_reference(env, it->second.connect_wrap_ctor_ref);
  if (it->second.tcp_binding_ref != nullptr) napi_delete_reference(env, it->second.tcp_binding_ref);
  g_tcp_states.erase(it);
}

void EnsureTcpCleanupHook(napi_env env) {
  if (env == nullptr) return;
  auto [it, inserted] = g_tcp_cleanup_hook_registered.emplace(env);
  if (!inserted) return;
  if (napi_add_env_cleanup_hook(env, OnTcpEnvCleanup, env) != napi_ok) {
    g_tcp_cleanup_hook_registered.erase(it);
  }
}

TcpBindingState* GetBindingState(napi_env env) {
  auto it = g_tcp_states.find(env);
  if (it == g_tcp_states.end()) return nullptr;
  return &it->second;
}

TcpBindingState& EnsureBindingState(napi_env env) {
  EnsureTcpCleanupHook(env);
  return g_tcp_states[env];
}

TcpWrap* FromBase(UbiStreamBase* base) {
  if (base == nullptr) return nullptr;
  return reinterpret_cast<TcpWrap*>(reinterpret_cast<char*>(base) - offsetof(TcpWrap, base));
}

uv_handle_t* TcpGetHandle(UbiStreamBase* base) {
  auto* wrap = FromBase(base);
  return wrap != nullptr ? reinterpret_cast<uv_handle_t*>(&wrap->handle) : nullptr;
}

uv_stream_t* TcpGetStreamForBase(UbiStreamBase* base) {
  auto* wrap = FromBase(base);
  return wrap != nullptr ? reinterpret_cast<uv_stream_t*>(&wrap->handle) : nullptr;
}

void TcpDestroy(UbiStreamBase* base) {
  delete FromBase(base);
}

void TcpAfterClose(uv_handle_t* handle) {
  auto* wrap = handle != nullptr ? static_cast<TcpWrap*>(handle->data) : nullptr;
  if (wrap == nullptr) return;
  UbiStreamBaseOnClosed(&wrap->base);
}

const UbiStreamBaseOps kTcpOps = {
    TcpGetHandle,
    TcpGetStreamForBase,
    TcpAfterClose,
    TcpDestroy,
    nullptr,
};

napi_value GetRefValue(napi_env env, napi_ref ref) {
  if (env == nullptr || ref == nullptr) return nullptr;
  napi_value value = nullptr;
  if (napi_get_reference_value(env, ref, &value) != napi_ok || value == nullptr) {
    return nullptr;
  }
  return value;
}

bool IsFunction(napi_env env, napi_value value) {
  if (env == nullptr || value == nullptr) return false;
  napi_valuetype type = napi_undefined;
  return napi_typeof(env, value, &type) == napi_ok && type == napi_function;
}

napi_value GetThis(napi_env env,
                   napi_callback_info info,
                   size_t* argc_out,
                   napi_value* argv,
                   TcpWrap** wrap_out) {
  size_t argc = argc_out != nullptr ? *argc_out : 0;
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  if (argc_out != nullptr) *argc_out = argc;
  if (wrap_out != nullptr) {
    *wrap_out = nullptr;
    if (self != nullptr) {
      napi_unwrap(env, self, reinterpret_cast<void**>(wrap_out));
    }
  }
  return self;
}

std::string ValueToUtf8(napi_env env, napi_value value) {
  if (env == nullptr || value == nullptr) return {};
  napi_value string_value = nullptr;
  if (napi_coerce_to_string(env, value, &string_value) != napi_ok || string_value == nullptr) return {};
  size_t len = 0;
  if (napi_get_value_string_utf8(env, string_value, nullptr, 0, &len) != napi_ok) return {};
  std::string out(len + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, string_value, out.data(), out.size(), &copied) != napi_ok) return {};
  out.resize(copied);
  return out;
}

void FillSockAddr(napi_env env, napi_value out, const sockaddr* sa) {
  if (env == nullptr || out == nullptr || sa == nullptr) return;
  char ip[INET6_ADDRSTRLEN] = {0};
  const char* family = "IPv4";
  int port = 0;
  if (sa->sa_family == AF_INET6) {
    family = "IPv6";
    const auto* a6 = reinterpret_cast<const sockaddr_in6*>(sa);
    uv_ip6_name(a6, ip, sizeof(ip));
    port = ntohs(a6->sin6_port);
  } else {
    const auto* a4 = reinterpret_cast<const sockaddr_in*>(sa);
    uv_ip4_name(a4, ip, sizeof(ip));
    port = ntohs(a4->sin_port);
  }
  napi_value address = nullptr;
  napi_value family_value = nullptr;
  napi_value port_value = nullptr;
  napi_create_string_utf8(env, ip, NAPI_AUTO_LENGTH, &address);
  napi_create_string_utf8(env, family, NAPI_AUTO_LENGTH, &family_value);
  napi_create_int32(env, port, &port_value);
  if (address != nullptr) napi_set_named_property(env, out, "address", address);
  if (family_value != nullptr) napi_set_named_property(env, out, "family", family_value);
  if (port_value != nullptr) napi_set_named_property(env, out, "port", port_value);
}

void OnAlloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
  auto* wrap = handle != nullptr ? static_cast<TcpWrap*>(handle->data) : nullptr;
  UbiStreamBaseOnUvAlloc(wrap != nullptr ? &wrap->base : nullptr, suggested_size, buf);
}

void OnRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
  auto* wrap = stream != nullptr ? static_cast<TcpWrap*>(stream->data) : nullptr;
  UbiStreamBaseOnUvRead(wrap != nullptr ? &wrap->base : nullptr, nread, buf);
}

void OnConnectDone(uv_connect_t* req, int status) {
  auto* cr = static_cast<ConnectReqWrap*>(req->data);
  if (cr == nullptr) return;
  napi_value req_obj = GetRefValue(cr->env, cr->req_obj_ref);
  napi_value tcp_obj = cr->tcp != nullptr ? UbiStreamBaseGetWrapper(&cr->tcp->base) : nullptr;
  napi_value argv[5] = {
      UbiStreamBaseMakeInt32(cr->env, status),
      tcp_obj,
      req_obj,
      UbiStreamBaseMakeBool(cr->env, true),
      UbiStreamBaseMakeBool(cr->env, true),
  };
  UbiStreamBaseInvokeReqOnComplete(cr->env, req_obj, status, argv, 5);
  if (cr->req_obj_ref != nullptr) napi_delete_reference(cr->env, cr->req_obj_ref);
  delete cr;
}

napi_value TcpCtor(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  if (self == nullptr) return nullptr;

  int32_t socket_type = kTcpSocket;
  if (argc >= 1 && argv[0] != nullptr) {
    napi_get_value_int32(env, argv[0], &socket_type);
  }

  auto* wrap = new TcpWrap();
  wrap->env = env;
  wrap->socket_type = socket_type;
  uv_loop_t* loop = UbiGetEnvLoop(env);
  if (loop == nullptr || uv_tcp_init(loop, &wrap->handle) != 0) {
    delete wrap;
    return UbiStreamBaseUndefined(env);
  }
  wrap->handle.data = wrap;
  const int32_t provider = (socket_type == kTcpServer) ? kUbiProviderTcpServerWrap : kUbiProviderTcpWrap;
  UbiStreamBaseInit(&wrap->base, env, &kTcpOps, provider);
  napi_wrap(env, self, wrap, [](napi_env finalize_env, void* data, void*) {
    auto* tcp_wrap = static_cast<TcpWrap*>(data);
    if (tcp_wrap == nullptr) return;
    UbiStreamBaseFinalize(&tcp_wrap->base);
  }, nullptr, &wrap->base.wrapper_ref);
  UbiStreamBaseSetWrapperRef(&wrap->base, wrap->base.wrapper_ref);
  UbiStreamBaseSetInitialStreamProperties(&wrap->base, true, true);
  return self;
}

napi_value TcpOpen(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  if (wrap == nullptr || argc < 1) return UbiStreamBaseMakeInt32(env, UV_EINVAL);
  int32_t fd = -1;
  napi_get_value_int32(env, argv[0], &fd);
  return UbiStreamBaseMakeInt32(env, uv_tcp_open(&wrap->handle, static_cast<uv_os_sock_t>(fd)));
}

napi_value TcpSetBlocking(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  if (wrap == nullptr || argc < 1) return UbiStreamBaseMakeInt32(env, UV_EINVAL);
  bool on = false;
  napi_get_value_bool(env, argv[0], &on);
  return UbiStreamBaseMakeInt32(env,
                                uv_stream_set_blocking(reinterpret_cast<uv_stream_t*>(&wrap->handle), on ? 1 : 0));
}

napi_value TcpBind(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  if (wrap == nullptr || argc < 2) return UbiStreamBaseMakeInt32(env, UV_EINVAL);
  std::string host = ValueToUtf8(env, argv[0]);
  int32_t port = 0;
  napi_get_value_int32(env, argv[1], &port);
  unsigned int flags = 0;
  if (argc > 2 && argv[2] != nullptr) {
    uint32_t tmp = 0;
    napi_get_value_uint32(env, argv[2], &tmp);
    flags = tmp;
#ifdef UV_TCP_IPV6ONLY
    flags &= ~UV_TCP_IPV6ONLY;
#endif
  }
  sockaddr_in addr{};
  int rc = uv_ip4_addr(host.c_str(), port, &addr);
  if (rc == 0) rc = uv_tcp_bind(&wrap->handle, reinterpret_cast<const sockaddr*>(&addr), flags);
  return UbiStreamBaseMakeInt32(env, rc);
}

napi_value TcpBind6(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  if (wrap == nullptr || argc < 2) return UbiStreamBaseMakeInt32(env, UV_EINVAL);
  std::string host = ValueToUtf8(env, argv[0]);
  int32_t port = 0;
  napi_get_value_int32(env, argv[1], &port);
  unsigned int flags = 0;
  if (argc > 2 && argv[2] != nullptr) {
    uint32_t tmp = 0;
    napi_get_value_uint32(env, argv[2], &tmp);
    flags = tmp;
  }
  sockaddr_in6 addr{};
  int rc = uv_ip6_addr(host.c_str(), port, &addr);
  if (rc == 0) rc = uv_tcp_bind(&wrap->handle, reinterpret_cast<const sockaddr*>(&addr), flags);
  return UbiStreamBaseMakeInt32(env, rc);
}

void OnConnection(uv_stream_t* server, int status) {
  auto* server_wrap = server != nullptr ? static_cast<TcpWrap*>(server->data) : nullptr;
  if (server_wrap == nullptr) return;
  napi_env env = server_wrap->env;
  napi_value server_obj = UbiStreamBaseGetWrapper(&server_wrap->base);
  napi_value onconnection = nullptr;
  if (server_obj == nullptr ||
      napi_get_named_property(env, server_obj, "onconnection", &onconnection) != napi_ok ||
      !IsFunction(env, onconnection)) {
    return;
  }

  napi_value argv[2] = {UbiStreamBaseMakeInt32(env, status), UbiStreamBaseUndefined(env)};
  if (status == 0) {
    napi_value ctor = UbiGetTcpWrapConstructor(env);
    napi_value arg = UbiStreamBaseMakeInt32(env, kTcpSocket);
    napi_value client_obj = nullptr;
    if (ctor == nullptr ||
        arg == nullptr ||
        napi_new_instance(env, ctor, 1, &arg, &client_obj) != napi_ok ||
        client_obj == nullptr) {
      return;
    }
    TcpWrap* client_wrap = nullptr;
    if (napi_unwrap(env, client_obj, reinterpret_cast<void**>(&client_wrap)) != napi_ok || client_wrap == nullptr) {
      return;
    }
    if (uv_accept(server, reinterpret_cast<uv_stream_t*>(&client_wrap->handle)) != 0) {
      return;
    }
    argv[1] = client_obj;
  }

  napi_value ignored = nullptr;
  UbiAsyncWrapMakeCallback(env,
                           server_wrap->base.async_id,
                           server_obj,
                           server_obj,
                           onconnection,
                           2,
                           argv,
                           &ignored,
                           kUbiMakeCallbackNone);
}

napi_value TcpListen(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  if (wrap == nullptr) return UbiStreamBaseMakeInt32(env, UV_EINVAL);
  int32_t backlog = 511;
  if (argc >= 1 && argv[0] != nullptr) napi_get_value_int32(env, argv[0], &backlog);
  return UbiStreamBaseMakeInt32(
      env,
      uv_listen(reinterpret_cast<uv_stream_t*>(&wrap->handle), backlog, OnConnection));
}

napi_value TcpConnectImpl(napi_env env,
                         TcpWrap* wrap,
                         napi_value req_obj,
                         const std::string& host,
                         int32_t port,
                         bool ipv6) {
  auto* cr = new ConnectReqWrap();
  cr->env = env;
  cr->tcp = wrap;
  cr->req.data = cr;
  napi_create_reference(env, req_obj, 1, &cr->req_obj_ref);

  int rc = 0;
  if (ipv6) {
    sockaddr_in6 addr6{};
    rc = uv_ip6_addr(host.c_str(), port, &addr6);
    if (rc == 0) {
      rc = uv_tcp_connect(&cr->req, &wrap->handle, reinterpret_cast<const sockaddr*>(&addr6), OnConnectDone);
    }
  } else {
    sockaddr_in addr4{};
    rc = uv_ip4_addr(host.c_str(), port, &addr4);
    if (rc == 0) {
      rc = uv_tcp_connect(&cr->req, &wrap->handle, reinterpret_cast<const sockaddr*>(&addr4), OnConnectDone);
    }
  }

  if (rc != 0) {
    UbiStreamBaseSetReqError(env, req_obj, rc);
    if (cr->req_obj_ref != nullptr) napi_delete_reference(env, cr->req_obj_ref);
    delete cr;
  }
  return UbiStreamBaseMakeInt32(env, rc);
}

napi_value TcpConnect(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  if (wrap == nullptr || argc < 2) return UbiStreamBaseMakeInt32(env, UV_EINVAL);
  int32_t port = -1;
  if (argc > 2 && argv[2] != nullptr) napi_get_value_int32(env, argv[2], &port);
  return TcpConnectImpl(env, wrap, argv[0], ValueToUtf8(env, argv[1]), port, false);
}

napi_value TcpConnect6(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  if (wrap == nullptr || argc < 3) return UbiStreamBaseMakeInt32(env, UV_EINVAL);
  int32_t port = 0;
  napi_get_value_int32(env, argv[2], &port);
  return TcpConnectImpl(env, wrap, argv[0], ValueToUtf8(env, argv[1]), port, true);
}

napi_value TcpShutdown(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  if (wrap == nullptr || argc < 1) return UbiStreamBaseMakeInt32(env, UV_EINVAL);
  return UbiLibuvStreamShutdown(&wrap->base, argv[0]);
}

napi_value TcpClose(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  if (wrap == nullptr) return UbiStreamBaseUndefined(env);
  return UbiStreamBaseClose(&wrap->base, argc > 0 ? argv[0] : nullptr);
}

napi_value TcpReadStart(napi_env env, napi_callback_info info) {
  TcpWrap* wrap = nullptr;
  GetThis(env, info, nullptr, nullptr, &wrap);
  if (wrap == nullptr) return UbiStreamBaseMakeInt32(env, UV_EINVAL);
  int rc = uv_read_start(reinterpret_cast<uv_stream_t*>(&wrap->handle), OnAlloc, OnRead);
  UbiStreamBaseSetReading(&wrap->base, rc == 0);
  return UbiStreamBaseMakeInt32(env, rc);
}

napi_value TcpReadStop(napi_env env, napi_callback_info info) {
  TcpWrap* wrap = nullptr;
  GetThis(env, info, nullptr, nullptr, &wrap);
  if (wrap == nullptr) return UbiStreamBaseMakeInt32(env, UV_EINVAL);
  int rc = uv_read_stop(reinterpret_cast<uv_stream_t*>(&wrap->handle));
  UbiStreamBaseSetReading(&wrap->base, false);
  return UbiStreamBaseMakeInt32(env, rc);
}

napi_value TcpWriteBuffer(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  if (wrap == nullptr || argc < 2) return UbiStreamBaseMakeInt32(env, UV_EINVAL);
  return UbiLibuvStreamWriteBuffer(&wrap->base, argv[0], argv[1], nullptr, nullptr);
}

napi_value TcpWriteString(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr};
  napi_value self = nullptr;
  void* data = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, &data);
  TcpWrap* wrap = nullptr;
  if (self != nullptr) {
    napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  }
  if (wrap == nullptr || argc < 2) return UbiStreamBaseMakeInt32(env, UV_EINVAL);
  return UbiLibuvStreamWriteString(&wrap->base,
                                   argv[0],
                                   argv[1],
                                   static_cast<const char*>(data),
                                   nullptr,
                                   nullptr);
}

napi_value TcpWritev(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  if (wrap == nullptr || argc < 2) return UbiStreamBaseMakeInt32(env, UV_EINVAL);
  bool all_buffers = false;
  if (argc > 2 && argv[2] != nullptr) napi_get_value_bool(env, argv[2], &all_buffers);
  return UbiLibuvStreamWriteV(&wrap->base, argv[0], argv[1], all_buffers, nullptr, nullptr);
}

napi_value TcpSetNoDelay(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  if (wrap == nullptr || argc < 1) return UbiStreamBaseMakeInt32(env, UV_EINVAL);
  bool on = false;
  napi_get_value_bool(env, argv[0], &on);
  return UbiStreamBaseMakeInt32(env, uv_tcp_nodelay(&wrap->handle, on ? 1 : 0));
}

napi_value TcpSetKeepAlive(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  if (wrap == nullptr || argc < 2) return UbiStreamBaseMakeInt32(env, UV_EINVAL);
  bool on = false;
  int32_t delay = 0;
  napi_get_value_bool(env, argv[0], &on);
  napi_get_value_int32(env, argv[1], &delay);
  return UbiStreamBaseMakeInt32(env,
                                uv_tcp_keepalive(&wrap->handle, on ? 1 : 0, static_cast<unsigned int>(delay)));
}

napi_value TcpRef(napi_env env, napi_callback_info info) {
  TcpWrap* wrap = nullptr;
  GetThis(env, info, nullptr, nullptr, &wrap);
  if (wrap != nullptr) UbiStreamBaseRef(&wrap->base);
  return UbiStreamBaseUndefined(env);
}

napi_value TcpUnref(napi_env env, napi_callback_info info) {
  TcpWrap* wrap = nullptr;
  GetThis(env, info, nullptr, nullptr, &wrap);
  if (wrap != nullptr) UbiStreamBaseUnref(&wrap->base);
  return UbiStreamBaseUndefined(env);
}

napi_value TcpHasRef(napi_env env, napi_callback_info info) {
  TcpWrap* wrap = nullptr;
  GetThis(env, info, nullptr, nullptr, &wrap);
  return UbiStreamBaseHasRefValue(wrap != nullptr ? &wrap->base : nullptr);
}

napi_value TcpGetAsyncId(napi_env env, napi_callback_info info) {
  TcpWrap* wrap = nullptr;
  GetThis(env, info, nullptr, nullptr, &wrap);
  return UbiStreamBaseGetAsyncId(wrap != nullptr ? &wrap->base : nullptr);
}

napi_value TcpGetProviderType(napi_env env, napi_callback_info info) {
  TcpWrap* wrap = nullptr;
  GetThis(env, info, nullptr, nullptr, &wrap);
  return UbiStreamBaseGetProviderType(wrap != nullptr ? &wrap->base : nullptr);
}

napi_value TcpAsyncReset(napi_env env, napi_callback_info info) {
  TcpWrap* wrap = nullptr;
  GetThis(env, info, nullptr, nullptr, &wrap);
  return UbiStreamBaseAsyncReset(wrap != nullptr ? &wrap->base : nullptr);
}

napi_value TcpReset(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  if (wrap == nullptr) return UbiStreamBaseMakeInt32(env, UV_EBADF);
  uv_handle_t* handle = reinterpret_cast<uv_handle_t*>(&wrap->handle);
  if (wrap->base.closed || wrap->base.closing || uv_is_closing(handle)) {
    return UbiStreamBaseMakeInt32(env, 0);
  }
  int rc = uv_tcp_close_reset(&wrap->handle, TcpAfterClose);
  if (rc == 0) {
    wrap->base.closing = true;
    if (argc > 0) UbiStreamBaseSetCloseCallback(&wrap->base, argv[0]);
  }
  return UbiStreamBaseMakeInt32(env, rc);
}

napi_value TcpUseUserBuffer(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  if (wrap == nullptr || argc < 1) return UbiStreamBaseMakeInt32(env, UV_EINVAL);
  return UbiStreamBaseUseUserBuffer(&wrap->base, argv[0]);
}

napi_value TcpGetSockName(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  if (wrap == nullptr || argc < 1) return UbiStreamBaseMakeInt32(env, UV_EINVAL);
  sockaddr_storage storage{};
  int len = sizeof(storage);
  int rc = uv_tcp_getsockname(&wrap->handle, reinterpret_cast<sockaddr*>(&storage), &len);
  if (rc == 0) FillSockAddr(env, argv[0], reinterpret_cast<const sockaddr*>(&storage));
  return UbiStreamBaseMakeInt32(env, rc);
}

napi_value TcpGetPeerName(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  if (wrap == nullptr || argc < 1) return UbiStreamBaseMakeInt32(env, UV_EINVAL);
  sockaddr_storage storage{};
  int len = sizeof(storage);
  int rc = uv_tcp_getpeername(&wrap->handle, reinterpret_cast<sockaddr*>(&storage), &len);
  if (rc == 0) FillSockAddr(env, argv[0], reinterpret_cast<const sockaddr*>(&storage));
  return UbiStreamBaseMakeInt32(env, rc);
}

napi_value TcpGetOnRead(napi_env env, napi_callback_info info) {
  TcpWrap* wrap = nullptr;
  GetThis(env, info, nullptr, nullptr, &wrap);
  return UbiStreamBaseGetOnRead(wrap != nullptr ? &wrap->base : nullptr);
}

napi_value TcpSetOnRead(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  return UbiStreamBaseSetOnRead(wrap != nullptr ? &wrap->base : nullptr, argc > 0 ? argv[0] : nullptr);
}

napi_value TcpBytesReadGetter(napi_env env, napi_callback_info info) {
  TcpWrap* wrap = nullptr;
  GetThis(env, info, nullptr, nullptr, &wrap);
  return UbiStreamBaseGetBytesRead(wrap != nullptr ? &wrap->base : nullptr);
}

napi_value TcpBytesWrittenGetter(napi_env env, napi_callback_info info) {
  TcpWrap* wrap = nullptr;
  GetThis(env, info, nullptr, nullptr, &wrap);
  return UbiStreamBaseGetBytesWritten(wrap != nullptr ? &wrap->base : nullptr);
}

napi_value TcpFdGetter(napi_env env, napi_callback_info info) {
  TcpWrap* wrap = nullptr;
  GetThis(env, info, nullptr, nullptr, &wrap);
  return UbiStreamBaseGetFd(wrap != nullptr ? &wrap->base : nullptr);
}

napi_value TcpExternalStreamGetter(napi_env env, napi_callback_info info) {
  TcpWrap* wrap = nullptr;
  GetThis(env, info, nullptr, nullptr, &wrap);
  return UbiStreamBaseGetExternal(wrap != nullptr ? &wrap->base : nullptr);
}

napi_value TcpWriteQueueSizeGetter(napi_env env, napi_callback_info info) {
  TcpWrap* wrap = nullptr;
  GetThis(env, info, nullptr, nullptr, &wrap);
  return UbiStreamBaseGetWriteQueueSize(wrap != nullptr ? &wrap->base : nullptr);
}

napi_value ConnectWrapCtor(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  return self;
}

void SetNamedU32(napi_env env, napi_value obj, const char* key, uint32_t value) {
  napi_value out = nullptr;
  napi_create_uint32(env, value, &out);
  if (out != nullptr) napi_set_named_property(env, obj, key, out);
}

}  // namespace

uv_stream_t* UbiTcpWrapGetStream(napi_env env, napi_value value) {
  if (env == nullptr || value == nullptr) return nullptr;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok || type != napi_object) return nullptr;
  TcpWrap* wrap = nullptr;
  if (napi_unwrap(env, value, reinterpret_cast<void**>(&wrap)) != napi_ok || wrap == nullptr) return nullptr;
  uv_handle_t* handle = reinterpret_cast<uv_handle_t*>(&wrap->handle);
  if (handle->data != wrap || handle->type != UV_TCP) return nullptr;
  return reinterpret_cast<uv_stream_t*>(&wrap->handle);
}

bool UbiTcpWrapPushStreamListener(uv_stream_t* stream, UbiStreamListener* listener) {
  if (stream == nullptr || listener == nullptr) return false;
  auto* wrap = static_cast<TcpWrap*>(stream->data);
  return wrap != nullptr && UbiStreamBasePushListener(&wrap->base, listener);
}

bool UbiTcpWrapRemoveStreamListener(uv_stream_t* stream, UbiStreamListener* listener) {
  if (stream == nullptr || listener == nullptr) return false;
  auto* wrap = static_cast<TcpWrap*>(stream->data);
  return wrap != nullptr && UbiStreamBaseRemoveListener(&wrap->base, listener);
}

napi_value UbiGetTcpWrapConstructor(napi_env env) {
  TcpBindingState* state = GetBindingState(env);
  napi_value ctor = state != nullptr ? GetRefValue(env, state->tcp_ctor_ref) : nullptr;
  if (ctor != nullptr) return ctor;
  napi_value binding = UbiInstallTcpWrapBinding(env);
  if (binding == nullptr) return nullptr;
  if (napi_get_named_property(env, binding, "TCP", &ctor) != napi_ok) return nullptr;
  return ctor;
}

napi_value UbiInstallTcpWrapBinding(napi_env env) {
  TcpBindingState& state = EnsureBindingState(env);
  napi_value cached = GetRefValue(env, state.tcp_binding_ref);
  if (cached != nullptr) return cached;

  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) return nullptr;

  napi_property_descriptor tcp_props[] = {
      {"open", nullptr, TcpOpen, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"setBlocking", nullptr, TcpSetBlocking, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"bind", nullptr, TcpBind, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"bind6", nullptr, TcpBind6, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"listen", nullptr, TcpListen, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"connect", nullptr, TcpConnect, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"connect6", nullptr, TcpConnect6, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"shutdown", nullptr, TcpShutdown, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"close", nullptr, TcpClose, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"readStart", nullptr, TcpReadStart, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"readStop", nullptr, TcpReadStop, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"writeBuffer", nullptr, TcpWriteBuffer, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"writev", nullptr, TcpWritev, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"writeLatin1String", nullptr, TcpWriteString, nullptr, nullptr, nullptr, napi_default_method,
       const_cast<char*>("latin1")},
      {"writeUtf8String", nullptr, TcpWriteString, nullptr, nullptr, nullptr, napi_default_method,
       const_cast<char*>("utf8")},
      {"writeAsciiString", nullptr, TcpWriteString, nullptr, nullptr, nullptr, napi_default_method,
       const_cast<char*>("ascii")},
      {"writeUcs2String", nullptr, TcpWriteString, nullptr, nullptr, nullptr, napi_default_method,
       const_cast<char*>("ucs2")},
      {"setNoDelay", nullptr, TcpSetNoDelay, nullptr, nullptr, nullptr,
       static_cast<napi_property_attributes>(napi_writable | napi_configurable), nullptr},
      {"setKeepAlive", nullptr, TcpSetKeepAlive, nullptr, nullptr, nullptr,
       static_cast<napi_property_attributes>(napi_writable | napi_configurable), nullptr},
      {"getsockname", nullptr, TcpGetSockName, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getpeername", nullptr, TcpGetPeerName, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"ref", nullptr, TcpRef, nullptr, nullptr, nullptr,
       static_cast<napi_property_attributes>(napi_writable | napi_configurable), nullptr},
      {"unref", nullptr, TcpUnref, nullptr, nullptr, nullptr,
       static_cast<napi_property_attributes>(napi_writable | napi_configurable), nullptr},
      {"hasRef", nullptr, TcpHasRef, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getAsyncId", nullptr, TcpGetAsyncId, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getProviderType", nullptr, TcpGetProviderType, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"asyncReset", nullptr, TcpAsyncReset, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"reset", nullptr, TcpReset, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"useUserBuffer", nullptr, TcpUseUserBuffer, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"onread", nullptr, nullptr, TcpGetOnRead, TcpSetOnRead, nullptr, napi_default, nullptr},
      {"bytesRead", nullptr, nullptr, TcpBytesReadGetter, nullptr, nullptr, napi_default, nullptr},
      {"bytesWritten", nullptr, nullptr, TcpBytesWrittenGetter, nullptr, nullptr, napi_default, nullptr},
      {"fd", nullptr, nullptr, TcpFdGetter, nullptr, nullptr, napi_default, nullptr},
      {"_externalStream", nullptr, nullptr, TcpExternalStreamGetter, nullptr, nullptr, napi_default, nullptr},
      {"writeQueueSize", nullptr, nullptr, TcpWriteQueueSizeGetter, nullptr, nullptr, napi_default, nullptr},
  };

  napi_value tcp_ctor = nullptr;
  if (napi_define_class(env,
                        "TCP",
                        NAPI_AUTO_LENGTH,
                        TcpCtor,
                        nullptr,
                        sizeof(tcp_props) / sizeof(tcp_props[0]),
                        tcp_props,
                        &tcp_ctor) != napi_ok ||
      tcp_ctor == nullptr) {
    return nullptr;
  }
  if (state.tcp_ctor_ref != nullptr) napi_delete_reference(env, state.tcp_ctor_ref);
  napi_create_reference(env, tcp_ctor, 1, &state.tcp_ctor_ref);

  napi_value connect_wrap_ctor = nullptr;
  if (napi_define_class(env,
                        "TCPConnectWrap",
                        NAPI_AUTO_LENGTH,
                        ConnectWrapCtor,
                        nullptr,
                        0,
                        nullptr,
                        &connect_wrap_ctor) != napi_ok ||
      connect_wrap_ctor == nullptr) {
    return nullptr;
  }
  if (state.connect_wrap_ctor_ref != nullptr) napi_delete_reference(env, state.connect_wrap_ctor_ref);
  napi_create_reference(env, connect_wrap_ctor, 1, &state.connect_wrap_ctor_ref);

  napi_value constants = nullptr;
  napi_create_object(env, &constants);
  SetNamedU32(env, constants, "SOCKET", kTcpSocket);
  SetNamedU32(env, constants, "SERVER", kTcpServer);
  SetNamedU32(env, constants, "UV_TCP_IPV6ONLY", static_cast<uint32_t>(UV_TCP_IPV6ONLY));
  SetNamedU32(env, constants, "UV_TCP_REUSEPORT", static_cast<uint32_t>(UV_TCP_REUSEPORT));

  napi_set_named_property(env, binding, "TCP", tcp_ctor);
  napi_set_named_property(env, binding, "TCPConnectWrap", connect_wrap_ctor);
  napi_set_named_property(env, binding, "constants", constants);

  if (state.tcp_binding_ref != nullptr) napi_delete_reference(env, state.tcp_binding_ref);
  napi_create_reference(env, binding, 1, &state.tcp_binding_ref);
  return binding;
}
