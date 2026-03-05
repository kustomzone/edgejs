#include "ubi_udp_wrap.h"

#include <arpa/inet.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <uv.h>
#if !defined(_WIN32)
#include <net/if.h>
#include <sys/socket.h>
#endif

#include "ubi_runtime.h"

namespace {

struct UdpWrap;

struct UdpSendReqWrap {
  uv_udp_send_t req{};
  napi_env env = nullptr;
  napi_ref req_obj_ref = nullptr;
  uv_buf_t* bufs = nullptr;
  uint32_t nbufs = 0;
  size_t msg_size = 0;
  bool have_callback = false;
  UdpWrap* udp = nullptr;
};

struct UdpWrap {
  napi_env env = nullptr;
  napi_ref wrapper_ref = nullptr;
  napi_ref close_cb_ref = nullptr;
  uv_udp_t handle{};
  bool closed = false;
  bool finalized = false;
  bool delete_on_close = false;
  bool has_ref = true;
  int64_t async_id = 200000;
};

struct SendWrap {
  napi_ref wrapper_ref = nullptr;
};

napi_ref g_udp_ctor_ref = nullptr;
int64_t g_next_async_id = 200000;

void OnClosed(uv_handle_t* h);

napi_value GetRefValue(napi_env env, napi_ref ref) {
  if (ref == nullptr) return nullptr;
  napi_value v = nullptr;
  if (napi_get_reference_value(env, ref, &v) != napi_ok) return nullptr;
  return v;
}

napi_value MakeInt32(napi_env env, int32_t v) {
  napi_value out = nullptr;
  napi_create_int32(env, v, &out);
  return out;
}

napi_value MakeBool(napi_env env, bool v) {
  napi_value out = nullptr;
  napi_get_boolean(env, v, &out);
  return out;
}

std::string ValueToUtf8(napi_env env, napi_value value) {
  size_t len = 0;
  if (napi_coerce_to_string(env, value, &value) != napi_ok ||
      napi_get_value_string_utf8(env, value, nullptr, 0, &len) != napi_ok) {
    return {};
  }
  std::string out(len + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, value, out.data(), out.size(), &copied) != napi_ok) return {};
  out.resize(copied);
  return out;
}

std::string FormatIPv6AddressWithScope(const sockaddr_in6* a6) {
  if (a6 == nullptr) return "";
  char ip[INET6_ADDRSTRLEN] = {0};
  uv_ip6_name(a6, ip, sizeof(ip));
  std::string out(ip);
  if (a6->sin6_scope_id == 0) return out;
#if defined(_WIN32)
  out += "%";
  out += std::to_string(static_cast<unsigned int>(a6->sin6_scope_id));
#else
  char ifname[IF_NAMESIZE] = {0};
  if (if_indextoname(a6->sin6_scope_id, ifname) != nullptr && ifname[0] != '\0') {
    out += "%";
    out += ifname;
  } else {
    out += "%";
    out += std::to_string(static_cast<unsigned int>(a6->sin6_scope_id));
  }
#endif
  return out;
}

size_t TypedArrayElementSize(napi_typedarray_type type) {
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

bool ReadUint32Property(napi_env env, napi_value obj, const char* key, uint32_t* out) {
  if (obj == nullptr || out == nullptr) return false;
  napi_value v = nullptr;
  if (napi_get_named_property(env, obj, key, &v) != napi_ok || v == nullptr) return false;
  return napi_get_value_uint32(env, v, out) == napi_ok;
}

bool ExtractArrayBufferViewBytes(napi_env env, napi_value value, const char** src, size_t* len) {
  if (value == nullptr || src == nullptr || len == nullptr) return false;
  *src = nullptr;
  *len = 0;

  bool is_typed = false;
  if (napi_is_typedarray(env, value, &is_typed) == napi_ok && is_typed) {
    napi_typedarray_type tt = napi_uint8_array;
    size_t element_len = 0;
    void* data = nullptr;
    napi_value ab = nullptr;
    size_t off = 0;
    if (napi_get_typedarray_info(env, value, &tt, &element_len, &data, &ab, &off) != napi_ok || data == nullptr) {
      return false;
    }
    *src = static_cast<const char*>(data);
    uint32_t byte_len = 0;
    if (ReadUint32Property(env, value, "byteLength", &byte_len)) {
      *len = static_cast<size_t>(byte_len);
    } else {
      *len = element_len * TypedArrayElementSize(tt);
    }
    return true;
  }

  bool is_dataview = false;
  if (napi_is_dataview(env, value, &is_dataview) == napi_ok && is_dataview) {
    void* data = nullptr;
    napi_value ab = nullptr;
    size_t off = 0;
    if (napi_get_dataview_info(env, value, len, &data, &ab, &off) != napi_ok || data == nullptr) return false;
    *src = static_cast<const char*>(data);
    return true;
  }

  napi_value ab = nullptr;
  if (napi_get_named_property(env, value, "buffer", &ab) == napi_ok && ab != nullptr) {
    bool is_arraybuffer = false;
    if (napi_is_arraybuffer(env, ab, &is_arraybuffer) == napi_ok && is_arraybuffer) {
      void* base = nullptr;
      size_t ab_len = 0;
      if (napi_get_arraybuffer_info(env, ab, &base, &ab_len) == napi_ok && base != nullptr) {
        uint32_t byte_offset = 0;
        uint32_t byte_len = 0;
        if (!ReadUint32Property(env, value, "byteOffset", &byte_offset)) byte_offset = 0;
        if (!ReadUint32Property(env, value, "byteLength", &byte_len)) return false;
        if (static_cast<size_t>(byte_offset) + static_cast<size_t>(byte_len) > ab_len) return false;
        *src = static_cast<const char*>(base) + byte_offset;
        *len = static_cast<size_t>(byte_len);
        return true;
      }
    }
  }

  bool is_arraybuffer = false;
  if (napi_is_arraybuffer(env, value, &is_arraybuffer) == napi_ok && is_arraybuffer) {
    void* data = nullptr;
    if (napi_get_arraybuffer_info(env, value, &data, len) != napi_ok || data == nullptr) return false;
    *src = static_cast<const char*>(data);
    return true;
  }

  return false;
}

void SetReqError(napi_env env, napi_value req_obj, int status) {
  if (req_obj == nullptr || status >= 0) return;
  const char* err = uv_err_name(status);
  napi_value err_v = nullptr;
  napi_create_string_utf8(env, err != nullptr ? err : "UV_ERROR", NAPI_AUTO_LENGTH, &err_v);
  if (err_v != nullptr) napi_set_named_property(env, req_obj, "error", err_v);
}

void SendWrapFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* wrap = static_cast<SendWrap*>(data);
  if (wrap == nullptr) return;
  if (wrap->wrapper_ref) napi_delete_reference(env, wrap->wrapper_ref);
  delete wrap;
}

void UdpFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* wrap = static_cast<UdpWrap*>(data);
  if (wrap == nullptr) return;
  wrap->finalized = true;
  if (wrap->wrapper_ref) {
    napi_delete_reference(env, wrap->wrapper_ref);
    wrap->wrapper_ref = nullptr;
  }
  if (wrap->close_cb_ref) {
    napi_delete_reference(env, wrap->close_cb_ref);
    wrap->close_cb_ref = nullptr;
  }
  uv_handle_t* h = reinterpret_cast<uv_handle_t*>(&wrap->handle);
  if (!wrap->closed) {
    wrap->delete_on_close = true;
    if (!uv_is_closing(h)) {
      uv_close(h, OnClosed);
    }
    return;
  }
  delete wrap;
}

napi_value SendWrapCtor(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  auto* wrap = new SendWrap();
  napi_wrap(env, self, wrap, SendWrapFinalize, nullptr, &wrap->wrapper_ref);
  return self;
}

napi_value UdpCtor(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  auto* wrap = new UdpWrap();
  wrap->env = env;
  wrap->async_id = g_next_async_id++;
  uv_udp_init(uv_default_loop(), &wrap->handle);
  wrap->handle.data = wrap;
  napi_wrap(env, self, wrap, UdpFinalize, nullptr, &wrap->wrapper_ref);
  // Node's internal/dgram mutates selected handle methods for udp6 aliases.
  const char* mutable_methods[] = {"bind", "bind6", "connect", "connect6", "send", "send6"};
  for (const char* key : mutable_methods) {
    napi_value fn = nullptr;
    if (napi_get_named_property(env, self, key, &fn) == napi_ok && fn != nullptr) {
      napi_property_descriptor desc = {key, nullptr, nullptr, nullptr, nullptr, fn,
                                       static_cast<napi_property_attributes>(napi_writable | napi_configurable),
                                       nullptr};
      napi_define_properties(env, self, 1, &desc);
    }
  }
  return self;
}

void InvokeReqOnComplete(napi_env env, napi_value req_obj, int status, uint32_t sent, bool have_callback) {
  if (req_obj == nullptr) return;
  SetReqError(env, req_obj, status);
  if (!have_callback) return;
  napi_value oncomplete = nullptr;
  if (napi_get_named_property(env, req_obj, "oncomplete", &oncomplete) != napi_ok || oncomplete == nullptr) return;
  napi_valuetype t = napi_undefined;
  napi_typeof(env, oncomplete, &t);
  if (t != napi_function) return;
  napi_value argv[2] = {MakeInt32(env, status), MakeInt32(env, static_cast<int32_t>(sent))};
  napi_value ignored = nullptr;
  UbiMakeCallback(env, req_obj, oncomplete, 2, argv, &ignored);
}

void OnSendDone(uv_udp_send_t* req, int status) {
  auto* sr = static_cast<UdpSendReqWrap*>(req->data);
  if (sr == nullptr) return;
  napi_value req_obj = GetRefValue(sr->env, sr->req_obj_ref);
  InvokeReqOnComplete(sr->env, req_obj, status, static_cast<uint32_t>(sr->msg_size), sr->have_callback);
  if (sr->bufs != nullptr) {
    for (uint32_t i = 0; i < sr->nbufs; i++) free(sr->bufs[i].base);
    delete[] sr->bufs;
  }
  if (sr->req_obj_ref) napi_delete_reference(sr->env, sr->req_obj_ref);
  delete sr;
}

void OnAlloc(uv_handle_t* /*h*/, size_t suggested_size, uv_buf_t* buf) {
  char* base = static_cast<char*>(malloc(suggested_size));
  *buf = uv_buf_init(base, static_cast<unsigned int>(suggested_size));
}

napi_value MakeBufferFromBytes(napi_env env, const char* data, size_t len) {
  void* out = nullptr;
  napi_value ab = nullptr;
  if (napi_create_arraybuffer(env, len, &out, &ab) != napi_ok || ab == nullptr) return nullptr;
  if (len > 0) {
    if (out == nullptr || data == nullptr) return nullptr;
    memcpy(out, data, len);
  }
  napi_value view = nullptr;
  if (napi_create_typedarray(env, napi_uint8_array, len, ab, 0, &view) != napi_ok || view == nullptr) return nullptr;

  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) return view;
  napi_value buffer_ctor = nullptr;
  if (napi_get_named_property(env, global, "Buffer", &buffer_ctor) != napi_ok || buffer_ctor == nullptr) return view;
  napi_value from_fn = nullptr;
  if (napi_get_named_property(env, buffer_ctor, "from", &from_fn) != napi_ok || from_fn == nullptr) return view;
  napi_valuetype t = napi_undefined;
  napi_typeof(env, from_fn, &t);
  if (t != napi_function) return view;
  napi_value argv[1] = {view};
  napi_value buf_obj = nullptr;
  if (napi_call_function(env, buffer_ctor, from_fn, 1, argv, &buf_obj) != napi_ok || buf_obj == nullptr) return view;
  return buf_obj;
}

void OnRecv(uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf, const sockaddr* addr, unsigned /*flags*/) {
  auto* wrap = static_cast<UdpWrap*>(handle->data);
  if (wrap == nullptr) {
    if (buf && buf->base) free(buf->base);
    return;
  }
  // Mirror Node/libuv behavior: ignore empty probe reads that have no address.
  // Zero-length datagrams still have an address and should be delivered.
  if (nread == 0 && addr == nullptr) {
    if (buf && buf->base) free(buf->base);
    return;
  }
  napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
  napi_value onmessage = nullptr;
  if (self != nullptr &&
      napi_get_named_property(wrap->env, self, "onmessage", &onmessage) == napi_ok &&
      onmessage != nullptr) {
    napi_valuetype t = napi_undefined;
    napi_typeof(wrap->env, onmessage, &t);
    if (t == napi_function) {
      napi_value argv[4] = {MakeInt32(wrap->env, static_cast<int32_t>(nread)), self, nullptr, nullptr};
      if (nread >= 0 && buf != nullptr && buf->base != nullptr) {
        argv[2] = MakeBufferFromBytes(wrap->env, buf->base, static_cast<size_t>(nread));
        if (argv[2] == nullptr) napi_get_undefined(wrap->env, &argv[2]);
      } else {
        napi_get_undefined(wrap->env, &argv[2]);
      }
      napi_value rinfo = nullptr;
      napi_create_object(wrap->env, &rinfo);
      if (addr != nullptr) {
        std::string ip;
        int port = 0;
        const char* fam = "IPv4";
        if (addr->sa_family == AF_INET6) {
          auto* a6 = reinterpret_cast<const sockaddr_in6*>(addr);
          ip = FormatIPv6AddressWithScope(a6);
          port = ntohs(a6->sin6_port);
          fam = "IPv6";
        } else {
          char ip4[INET6_ADDRSTRLEN] = {0};
          auto* a4 = reinterpret_cast<const sockaddr_in*>(addr);
          uv_ip4_name(a4, ip4, sizeof(ip4));
          ip = ip4;
          port = ntohs(a4->sin_port);
        }
        napi_value ip_v = nullptr;
        napi_value fam_v = nullptr;
        napi_value port_v = nullptr;
        napi_value size_v = nullptr;
        napi_create_string_utf8(wrap->env, ip.c_str(), NAPI_AUTO_LENGTH, &ip_v);
        napi_create_string_utf8(wrap->env, fam, NAPI_AUTO_LENGTH, &fam_v);
        napi_create_int32(wrap->env, port, &port_v);
        napi_create_int32(wrap->env, nread >= 0 ? static_cast<int32_t>(nread) : 0, &size_v);
        if (ip_v) napi_set_named_property(wrap->env, rinfo, "address", ip_v);
        if (fam_v) napi_set_named_property(wrap->env, rinfo, "family", fam_v);
        if (port_v) napi_set_named_property(wrap->env, rinfo, "port", port_v);
        if (size_v) napi_set_named_property(wrap->env, rinfo, "size", size_v);
      }
      if (rinfo == nullptr) napi_get_undefined(wrap->env, &rinfo);
      argv[3] = rinfo;
      napi_value ignored = nullptr;
      UbiMakeCallback(wrap->env, self, onmessage, 4, argv, &ignored);
    }
  }
  if (buf && buf->base) free(buf->base);
}

void OnClosed(uv_handle_t* h) {
  auto* wrap = static_cast<UdpWrap*>(h->data);
  if (wrap == nullptr) return;
  wrap->closed = true;
  if (!wrap->finalized && wrap->close_cb_ref) {
    napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
    napi_value cb = GetRefValue(wrap->env, wrap->close_cb_ref);
    if (cb != nullptr) {
      napi_value ignored = nullptr;
      UbiMakeCallback(wrap->env, self, cb, 0, nullptr, &ignored);
    }
    napi_delete_reference(wrap->env, wrap->close_cb_ref);
    wrap->close_cb_ref = nullptr;
  }
  if (wrap->delete_on_close || wrap->finalized) {
    delete wrap;
  }
}

napi_value UdpBindImpl(napi_env env, UdpWrap* wrap, napi_value ip_val, int32_t port, bool ipv6, uint32_t flags) {
  std::string ip = ValueToUtf8(env, ip_val);
  int rc = 0;
  if (ipv6) {
    sockaddr_in6 a6{};
    rc = uv_ip6_addr(ip.c_str(), port, &a6);
    if (rc == 0) rc = uv_udp_bind(&wrap->handle, reinterpret_cast<const sockaddr*>(&a6), flags);
  } else {
    sockaddr_in a4{};
    rc = uv_ip4_addr(ip.c_str(), port, &a4);
    if (rc == 0) rc = uv_udp_bind(&wrap->handle, reinterpret_cast<const sockaddr*>(&a4), flags);
  }
  return MakeInt32(env, rc);
}

napi_value UdpBind(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr || argc < 2) return MakeInt32(env, UV_EINVAL);
  int32_t port = 0;
  napi_get_value_int32(env, argv[1], &port);
  uint32_t flags = 0;
  if (argc > 2 && argv[2] != nullptr) napi_get_value_uint32(env, argv[2], &flags);
  return UdpBindImpl(env, wrap, argv[0], port, false, flags);
}

napi_value UdpBind6(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr || argc < 2) return MakeInt32(env, UV_EINVAL);
  int32_t port = 0;
  napi_get_value_int32(env, argv[1], &port);
  uint32_t flags = 0;
  if (argc > 2 && argv[2] != nullptr) napi_get_value_uint32(env, argv[2], &flags);
  return UdpBindImpl(env, wrap, argv[0], port, true, flags);
}

napi_value UdpOpen(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr || argc < 1) return MakeInt32(env, UV_EINVAL);
  int32_t fd = -1;
  napi_get_value_int32(env, argv[0], &fd);
  int rc = uv_udp_open(&wrap->handle, static_cast<uv_os_sock_t>(fd));
  return MakeInt32(env, rc);
}

napi_value UdpRecvStart(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr) return MakeInt32(env, UV_EINVAL);
  int rc = uv_udp_recv_start(&wrap->handle, OnAlloc, OnRecv);
  return MakeInt32(env, rc);
}

napi_value UdpRecvStop(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr) return MakeInt32(env, UV_EINVAL);
  int rc = uv_udp_recv_stop(&wrap->handle);
  return MakeInt32(env, rc);
}

napi_value UdpClose(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr) {
    napi_value u = nullptr;
    napi_get_undefined(env, &u);
    return u;
  }
  if (argc > 0 && argv[0] != nullptr) {
    napi_valuetype t = napi_undefined;
    napi_typeof(env, argv[0], &t);
    if (t == napi_function) {
      if (wrap->close_cb_ref) napi_delete_reference(env, wrap->close_cb_ref);
      napi_create_reference(env, argv[0], 1, &wrap->close_cb_ref);
    }
  }
  if (!wrap->closed && !uv_is_closing(reinterpret_cast<uv_handle_t*>(&wrap->handle))) {
    uv_close(reinterpret_cast<uv_handle_t*>(&wrap->handle), OnClosed);
  }
  napi_value u = nullptr;
  napi_get_undefined(env, &u);
  return u;
}

napi_value UdpSend(napi_env env, napi_callback_info info) {
  size_t argc = 6;
  napi_value argv[6] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr || argc < 3) return MakeInt32(env, UV_EINVAL);

  napi_value req_obj = argv[0];
  napi_value list = argv[1];
  uint32_t list_len = 0;
  napi_get_array_length(env, list, &list_len);

  auto* sr = new UdpSendReqWrap();
  sr->env = env;
  sr->udp = wrap;
  sr->req.data = sr;
  sr->nbufs = list_len;
  sr->msg_size = 0;
  bool have_callback = false;
  if (argc >= 6 && argv[5] != nullptr) {
    napi_get_value_bool(env, argv[5], &have_callback);
  } else if (argc >= 4 && argv[3] != nullptr) {
    napi_get_value_bool(env, argv[3], &have_callback);
  }
  sr->have_callback = have_callback;
  sr->bufs = new uv_buf_t[list_len > 0 ? list_len : 1];
  napi_create_reference(env, req_obj, 1, &sr->req_obj_ref);

  for (uint32_t i = 0; i < list_len; i++) {
    napi_value chunk = nullptr;
    napi_get_element(env, list, i, &chunk);
    const char* src = nullptr;
    size_t len = 0;
    std::string tmp;
    if (!ExtractArrayBufferViewBytes(env, chunk, &src, &len)) {
      tmp = ValueToUtf8(env, chunk);
      src = tmp.data();
      len = tmp.size();
    }
    // Keep a stable non-null base pointer even for zero-length datagrams.
    // Some libuv/platform paths may still dereference base regardless of len.
    char* copy = static_cast<char*>(malloc(len > 0 ? len : 1));
    if (len > 0 && src != nullptr) memcpy(copy, src, len);
    sr->bufs[i] = uv_buf_init(copy, static_cast<unsigned int>(len));
    sr->msg_size += len;
  }

  sockaddr_storage ss{};
  const sockaddr* send_addr = nullptr;
  int rc = 0;
  if (argc >= 5 && argv[3] != nullptr && argv[4] != nullptr) {
    int32_t port = 0;
    napi_get_value_int32(env, argv[3], &port);
    std::string ip = ValueToUtf8(env, argv[4]);
    if (ip.find(':') != std::string::npos) {
      auto* a6 = reinterpret_cast<sockaddr_in6*>(&ss);
      uv_ip6_addr(ip.c_str(), port, a6);
      send_addr = reinterpret_cast<const sockaddr*>(a6);
    } else {
      auto* a4 = reinterpret_cast<sockaddr_in*>(&ss);
      uv_ip4_addr(ip.c_str(), port, a4);
      send_addr = reinterpret_cast<const sockaddr*>(a4);
    }
  }
  uv_buf_t* send_bufs = sr->bufs;
  uint32_t send_count = sr->nbufs;
  if (sr->msg_size > 0) {
    rc = uv_udp_try_send(&wrap->handle, send_bufs, send_count, send_addr);
    if (rc == UV_ENOSYS || rc == UV_EAGAIN) {
      rc = 0;
    } else if (rc >= 0) {
      size_t sent = static_cast<size_t>(rc);
      while (send_count > 0 && send_bufs->len <= sent) {
        sent -= send_bufs->len;
        send_bufs++;
        send_count--;
      }
      if (send_count == 0) {
        const int32_t sync_success = static_cast<int32_t>(sr->msg_size + 1);
        for (uint32_t i = 0; i < sr->nbufs; i++) free(sr->bufs[i].base);
        delete[] sr->bufs;
        napi_delete_reference(env, sr->req_obj_ref);
        delete sr;
        return MakeInt32(env, sync_success);
      }
      if (sent > 0) {
        send_bufs->base += sent;
        send_bufs->len -= sent;
      }
      rc = 0;
    }
  } else {
    rc = 0;
  }
  if (rc == 0) {
    rc = uv_udp_send(&sr->req, &wrap->handle, send_bufs, send_count, send_addr, OnSendDone);
  }
  if (rc != 0) {
    SetReqError(env, req_obj, rc);
    for (uint32_t i = 0; i < sr->nbufs; i++) free(sr->bufs[i].base);
    delete[] sr->bufs;
    napi_delete_reference(env, sr->req_obj_ref);
    delete sr;
  }
  return MakeInt32(env, rc);
}

napi_value UdpSend6(napi_env env, napi_callback_info info) {
  return UdpSend(env, info);
}

napi_value UdpGetSockName(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr || argc < 1) return MakeInt32(env, UV_EINVAL);
  sockaddr_storage ss{};
  int len = sizeof(ss);
  int rc = uv_udp_getsockname(&wrap->handle, reinterpret_cast<sockaddr*>(&ss), &len);
  if (rc == 0) {
    std::string ip;
    int port = 0;
    const char* fam = "IPv4";
    if (ss.ss_family == AF_INET6) {
      auto* a6 = reinterpret_cast<sockaddr_in6*>(&ss);
      ip = FormatIPv6AddressWithScope(a6);
      port = ntohs(a6->sin6_port);
      fam = "IPv6";
    } else {
      char ip4[INET6_ADDRSTRLEN] = {0};
      auto* a4 = reinterpret_cast<sockaddr_in*>(&ss);
      uv_ip4_name(a4, ip4, sizeof(ip4));
      ip = ip4;
      port = ntohs(a4->sin_port);
    }
    napi_value ip_v = nullptr;
    napi_value fam_v = nullptr;
    napi_value port_v = nullptr;
    napi_create_string_utf8(env, ip.c_str(), NAPI_AUTO_LENGTH, &ip_v);
    napi_create_string_utf8(env, fam, NAPI_AUTO_LENGTH, &fam_v);
    napi_create_int32(env, port, &port_v);
    if (ip_v) napi_set_named_property(env, argv[0], "address", ip_v);
    if (fam_v) napi_set_named_property(env, argv[0], "family", fam_v);
    if (port_v) napi_set_named_property(env, argv[0], "port", port_v);
  }
  return MakeInt32(env, rc);
}

napi_value UdpRef(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap != nullptr) {
    uv_ref(reinterpret_cast<uv_handle_t*>(&wrap->handle));
    wrap->has_ref = true;
  }
  napi_value u = nullptr;
  napi_get_undefined(env, &u);
  return u;
}

napi_value UdpUnref(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap != nullptr) {
    uv_unref(reinterpret_cast<uv_handle_t*>(&wrap->handle));
    wrap->has_ref = false;
  }
  napi_value u = nullptr;
  napi_get_undefined(env, &u);
  return u;
}

napi_value UdpHasRef(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  return MakeBool(env, wrap != nullptr ? wrap->has_ref : false);
}

napi_value UdpGetAsyncId(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  napi_value out = nullptr;
  napi_create_int64(env, wrap != nullptr ? wrap->async_id : -1, &out);
  return out;
}

napi_value UdpFdGetter(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  int32_t fd = -1;
  if (wrap != nullptr) {
    uv_os_fd_t raw = -1;
    if (uv_fileno(reinterpret_cast<const uv_handle_t*>(&wrap->handle), &raw) == 0) {
      fd = static_cast<int32_t>(raw);
    }
  }
  return MakeInt32(env, fd);
}

napi_value UdpSetMulticastAll(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr || argc < 1) return MakeInt32(env, UV_EINVAL);
  bool on = false;
  if (napi_get_value_bool(env, argv[0], &on) != napi_ok) return MakeInt32(env, UV_EINVAL);

#if !defined(_WIN32) && defined(IP_MULTICAST_ALL)
  uv_os_fd_t fd = 0;
  const int fileno_rc = uv_fileno(reinterpret_cast<const uv_handle_t*>(&wrap->handle), &fd);
  if (fileno_rc != 0) return MakeInt32(env, fileno_rc);
  int value = on ? 1 : 0;
  if (setsockopt(static_cast<int>(fd), IPPROTO_IP, IP_MULTICAST_ALL, &value, sizeof(value)) != 0) {
    return MakeInt32(env, uv_translate_sys_error(errno));
  }
#else
  (void)on;
#endif
  return MakeInt32(env, 0);
}

napi_value UdpBufferSizeCompat(napi_env env, napi_callback_info info, bool recv, bool set_mode) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr) {
    napi_value undef = nullptr;
    napi_get_undefined(env, &undef);
    return undef;
  }

  int value = 0;
  if (set_mode) {
    if (argc < 1 || argv[0] == nullptr || napi_get_value_int32(env, argv[0], &value) != napi_ok) {
      napi_value undef = nullptr;
      napi_get_undefined(env, &undef);
      return undef;
    }
  }

  const int rc = recv ? uv_recv_buffer_size(reinterpret_cast<uv_handle_t*>(&wrap->handle), &value)
                      : uv_send_buffer_size(reinterpret_cast<uv_handle_t*>(&wrap->handle), &value);
  if (rc != 0) {
    napi_value undef = nullptr;
    napi_get_undefined(env, &undef);
    return undef;
  }
  return MakeInt32(env, value);
}

napi_value UdpSetRecvBufferSize(napi_env env, napi_callback_info info) {
  return UdpBufferSizeCompat(env, info, true, true);
}

napi_value UdpSetSendBufferSize(napi_env env, napi_callback_info info) {
  return UdpBufferSizeCompat(env, info, false, true);
}

napi_value UdpGetRecvBufferSize(napi_env env, napi_callback_info info) {
  return UdpBufferSizeCompat(env, info, true, false);
}

napi_value UdpGetSendBufferSize(napi_env env, napi_callback_info info) {
  return UdpBufferSizeCompat(env, info, false, false);
}

napi_value UdpSetBroadcast(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr || argc < 1) return MakeInt32(env, UV_EINVAL);
  bool on = false;
  napi_get_value_bool(env, argv[0], &on);
  return MakeInt32(env, uv_udp_set_broadcast(&wrap->handle, on ? 1 : 0));
}

napi_value UdpSetTTL(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr || argc < 1) return MakeInt32(env, UV_EINVAL);
  int32_t ttl = 0;
  napi_get_value_int32(env, argv[0], &ttl);
  return MakeInt32(env, uv_udp_set_ttl(&wrap->handle, ttl));
}

napi_value UdpSetMulticastTTL(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr || argc < 1) return MakeInt32(env, UV_EINVAL);
  int32_t ttl = 0;
  napi_get_value_int32(env, argv[0], &ttl);
  return MakeInt32(env, uv_udp_set_multicast_ttl(&wrap->handle, ttl));
}

napi_value UdpSetMulticastLoopback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr || argc < 1) return MakeInt32(env, UV_EINVAL);
  bool on = false;
  napi_get_value_bool(env, argv[0], &on);
  return MakeInt32(env, uv_udp_set_multicast_loop(&wrap->handle, on ? 1 : 0));
}

napi_value UdpSetMulticastInterface(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr || argc < 1) return MakeInt32(env, UV_EINVAL);
  std::string iface = ValueToUtf8(env, argv[0]);
  return MakeInt32(env, uv_udp_set_multicast_interface(&wrap->handle, iface.c_str()));
}

napi_value UdpMembershipImpl(napi_env env, napi_callback_info info, uv_membership membership) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr || argc < 1 || argv[0] == nullptr) return MakeInt32(env, UV_EINVAL);
  std::string multicast = ValueToUtf8(env, argv[0]);
  const char* iface = nullptr;
  std::string iface_storage;
  if (argc > 1 && argv[1] != nullptr) {
    iface_storage = ValueToUtf8(env, argv[1]);
    iface = iface_storage.c_str();
  }
  return MakeInt32(env, uv_udp_set_membership(&wrap->handle, multicast.c_str(), iface, membership));
}

napi_value UdpAddMembership(napi_env env, napi_callback_info info) {
  return UdpMembershipImpl(env, info, UV_JOIN_GROUP);
}

napi_value UdpDropMembership(napi_env env, napi_callback_info info) {
  return UdpMembershipImpl(env, info, UV_LEAVE_GROUP);
}

napi_value UdpSourceMembershipImpl(napi_env env,
                                   napi_callback_info info,
                                   uv_membership membership) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr || argc < 2 || argv[0] == nullptr || argv[1] == nullptr) {
    return MakeInt32(env, UV_EINVAL);
  }
#if UV_VERSION_MAJOR > 1 || (UV_VERSION_MAJOR == 1 && UV_VERSION_MINOR >= 32)
  std::string source = ValueToUtf8(env, argv[0]);
  std::string group = ValueToUtf8(env, argv[1]);
  const char* iface = nullptr;
  std::string iface_storage;
  if (argc > 2 && argv[2] != nullptr) {
    iface_storage = ValueToUtf8(env, argv[2]);
    iface = iface_storage.c_str();
  }
  return MakeInt32(env,
                   uv_udp_set_source_membership(
                       &wrap->handle, group.c_str(), iface, source.c_str(), membership));
#else
  return MakeInt32(env, UV_ENOTSUP);
#endif
}

napi_value UdpAddSourceSpecificMembership(napi_env env, napi_callback_info info) {
  return UdpSourceMembershipImpl(env, info, UV_JOIN_GROUP);
}

napi_value UdpDropSourceSpecificMembership(napi_env env, napi_callback_info info) {
  return UdpSourceMembershipImpl(env, info, UV_LEAVE_GROUP);
}

napi_value UdpConnectImpl(napi_env env, napi_callback_info info, bool ipv6) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr || argc < 2) return MakeInt32(env, UV_EINVAL);
  std::string host = ValueToUtf8(env, argv[0]);
  int32_t port = 0;
  napi_get_value_int32(env, argv[1], &port);
  if (ipv6) {
    sockaddr_in6 a6{};
    int rc = uv_ip6_addr(host.c_str(), port, &a6);
    if (rc != 0) return MakeInt32(env, rc);
    return MakeInt32(env, uv_udp_connect(&wrap->handle, reinterpret_cast<const sockaddr*>(&a6)));
  }
  sockaddr_in a4{};
  int rc = uv_ip4_addr(host.c_str(), port, &a4);
  if (rc != 0) return MakeInt32(env, rc);
  return MakeInt32(env, uv_udp_connect(&wrap->handle, reinterpret_cast<const sockaddr*>(&a4)));
}

napi_value UdpConnect(napi_env env, napi_callback_info info) {
  return UdpConnectImpl(env, info, false);
}

napi_value UdpConnect6(napi_env env, napi_callback_info info) {
  return UdpConnectImpl(env, info, true);
}

napi_value UdpDisconnect(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr) return MakeInt32(env, UV_EINVAL);
  return MakeInt32(env, uv_udp_connect(&wrap->handle, nullptr));
}

napi_value UdpGetPeerName(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr || argc < 1) return MakeInt32(env, UV_EINVAL);
  sockaddr_storage ss{};
  int len = sizeof(ss);
  int rc = uv_udp_getpeername(&wrap->handle, reinterpret_cast<sockaddr*>(&ss), &len);
  if (rc == 0) {
    std::string ip;
    int port = 0;
    const char* fam = "IPv4";
    if (ss.ss_family == AF_INET6) {
      auto* a6 = reinterpret_cast<sockaddr_in6*>(&ss);
      ip = FormatIPv6AddressWithScope(a6);
      port = ntohs(a6->sin6_port);
      fam = "IPv6";
    } else {
      char ip4[INET6_ADDRSTRLEN] = {0};
      auto* a4 = reinterpret_cast<sockaddr_in*>(&ss);
      uv_ip4_name(a4, ip4, sizeof(ip4));
      ip = ip4;
      port = ntohs(a4->sin_port);
    }
    napi_value ip_v = nullptr;
    napi_value fam_v = nullptr;
    napi_value port_v = nullptr;
    napi_create_string_utf8(env, ip.c_str(), NAPI_AUTO_LENGTH, &ip_v);
    napi_create_string_utf8(env, fam, NAPI_AUTO_LENGTH, &fam_v);
    napi_create_int32(env, port, &port_v);
    if (ip_v) napi_set_named_property(env, argv[0], "address", ip_v);
    if (fam_v) napi_set_named_property(env, argv[0], "family", fam_v);
    if (port_v) napi_set_named_property(env, argv[0], "port", port_v);
  }
  return MakeInt32(env, rc);
}

napi_value UdpBufferSize(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr || argc < 2) {
    napi_value undef = nullptr;
    napi_get_undefined(env, &undef);
    return undef;
  }
  int32_t size = 0;
  napi_get_value_int32(env, argv[0], &size);
  bool recv = false;
  napi_get_value_bool(env, argv[1], &recv);
  int value = size;
  const char* syscall = recv ? "uv_recv_buffer_size" : "uv_send_buffer_size";
  int rc = recv
      ? uv_recv_buffer_size(reinterpret_cast<uv_handle_t*>(&wrap->handle), &value)
      : uv_send_buffer_size(reinterpret_cast<uv_handle_t*>(&wrap->handle), &value);
  if (rc != 0) {
    if (argc > 2 && argv[2] != nullptr) {
      napi_value errno_v = nullptr;
      napi_create_int32(env, rc, &errno_v);
      if (errno_v) napi_set_named_property(env, argv[2], "errno", errno_v);
      const char* code = uv_err_name(rc);
      napi_value code_v = nullptr;
      napi_create_string_utf8(env, code ? code : "UV_ERROR", NAPI_AUTO_LENGTH, &code_v);
      if (code_v) napi_set_named_property(env, argv[2], "code", code_v);
      const char* msg = uv_strerror(rc);
      napi_value msg_v = nullptr;
      napi_create_string_utf8(env, msg ? msg : "buffer size error", NAPI_AUTO_LENGTH, &msg_v);
      if (msg_v) napi_set_named_property(env, argv[2], "message", msg_v);
      napi_value syscall_v = nullptr;
      napi_create_string_utf8(env, syscall, NAPI_AUTO_LENGTH, &syscall_v);
      if (syscall_v) napi_set_named_property(env, argv[2], "syscall", syscall_v);
    }
    napi_value undef = nullptr;
    napi_get_undefined(env, &undef);
    return undef;
  }
  return MakeInt32(env, value);
}

napi_value UdpGetSendQueueSize(napi_env env, napi_callback_info info) {
#if UV_VERSION_MAJOR > 1 || (UV_VERSION_MAJOR == 1 && UV_VERSION_MINOR >= 19)
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  uint64_t value = wrap != nullptr ? uv_udp_get_send_queue_size(&wrap->handle) : 0;
  napi_value out = nullptr;
  napi_create_double(env, static_cast<double>(value), &out);
  return out;
#else
  return MakeInt32(env, 0);
#endif
}

napi_value UdpGetSendQueueCount(napi_env env, napi_callback_info info) {
#if UV_VERSION_MAJOR > 1 || (UV_VERSION_MAJOR == 1 && UV_VERSION_MINOR >= 19)
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  size_t value = wrap != nullptr ? uv_udp_get_send_queue_count(&wrap->handle) : 0;
  napi_value out = nullptr;
  napi_create_double(env, static_cast<double>(value), &out);
  return out;
#else
  return MakeInt32(env, 0);
#endif
}

void SetNamedU32(napi_env env, napi_value obj, const char* key, uint32_t value) {
  napi_value v = nullptr;
  napi_create_uint32(env, value, &v);
  if (v != nullptr) napi_set_named_property(env, obj, key, v);
}

}  // namespace

napi_value UbiInstallUdpWrapBinding(napi_env env) {
  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) return nullptr;

  napi_property_descriptor udp_props[] = {
      {"open", nullptr, UdpOpen, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"bind", nullptr, UdpBind, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"bind6", nullptr, UdpBind6, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"send", nullptr, UdpSend, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"send6", nullptr, UdpSend6, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"recvStart", nullptr, UdpRecvStart, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"recvStop", nullptr, UdpRecvStop, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getsockname", nullptr, UdpGetSockName, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getpeername", nullptr, UdpGetPeerName, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"close", nullptr, UdpClose, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"setBroadcast", nullptr, UdpSetBroadcast, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"setTTL", nullptr, UdpSetTTL, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"setMulticastTTL", nullptr, UdpSetMulticastTTL, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"setMulticastLoopback", nullptr, UdpSetMulticastLoopback, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"setMulticastInterface", nullptr, UdpSetMulticastInterface, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"addMembership", nullptr, UdpAddMembership, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"dropMembership", nullptr, UdpDropMembership, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"addSourceSpecificMembership", nullptr, UdpAddSourceSpecificMembership, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"dropSourceSpecificMembership", nullptr, UdpDropSourceSpecificMembership, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"setMulticastAll", nullptr, UdpSetMulticastAll, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"bufferSize", nullptr, UdpBufferSize, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"setRecvBufferSize", nullptr, UdpSetRecvBufferSize, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"setSendBufferSize", nullptr, UdpSetSendBufferSize, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getRecvBufferSize", nullptr, UdpGetRecvBufferSize, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getSendBufferSize", nullptr, UdpGetSendBufferSize, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"connect", nullptr, UdpConnect, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"connect6", nullptr, UdpConnect6, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"disconnect", nullptr, UdpDisconnect, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"ref", nullptr, UdpRef, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"unref", nullptr, UdpUnref, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"hasRef", nullptr, UdpHasRef, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getAsyncId", nullptr, UdpGetAsyncId, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"fd", nullptr, nullptr, UdpFdGetter, nullptr, nullptr, napi_default, nullptr},
      {"getSendQueueSize", nullptr, UdpGetSendQueueSize, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getSendQueueCount", nullptr, UdpGetSendQueueCount, nullptr, nullptr, nullptr, napi_default_method, nullptr},
  };
  napi_value udp_ctor = nullptr;
  if (napi_define_class(env,
                        "UDP",
                        NAPI_AUTO_LENGTH,
                        UdpCtor,
                        nullptr,
                        sizeof(udp_props) / sizeof(udp_props[0]),
                        udp_props,
                        &udp_ctor) != napi_ok ||
      udp_ctor == nullptr) {
    return nullptr;
  }
  if (g_udp_ctor_ref != nullptr) napi_delete_reference(env, g_udp_ctor_ref);
  napi_create_reference(env, udp_ctor, 1, &g_udp_ctor_ref);

  napi_value send_wrap_ctor = nullptr;
  if (napi_define_class(env, "SendWrap", NAPI_AUTO_LENGTH, SendWrapCtor, nullptr, 0, nullptr, &send_wrap_ctor) != napi_ok ||
      send_wrap_ctor == nullptr) {
    return nullptr;
  }

  napi_value constants = nullptr;
  napi_create_object(env, &constants);
  SetNamedU32(env, constants, "UV_UDP_IPV6ONLY", UV_UDP_IPV6ONLY);
  SetNamedU32(env, constants, "UV_UDP_REUSEPORT", UV_UDP_REUSEPORT);

  napi_set_named_property(env, binding, "UDP", udp_ctor);
  napi_set_named_property(env, binding, "SendWrap", send_wrap_ctor);
  napi_set_named_property(env, binding, "constants", constants);

  return binding;
}

napi_value UbiGetUdpWrapConstructor(napi_env env) {
  if (env == nullptr || g_udp_ctor_ref == nullptr) return nullptr;
  return GetRefValue(env, g_udp_ctor_ref);
}

uv_handle_t* UbiUdpWrapGetHandle(napi_env env, napi_value value) {
  if (env == nullptr || value == nullptr) return nullptr;
  void* raw = nullptr;
  if (napi_unwrap(env, value, &raw) != napi_ok || raw == nullptr) return nullptr;
  auto* wrap = static_cast<UdpWrap*>(raw);
  return reinterpret_cast<uv_handle_t*>(&wrap->handle);
}
