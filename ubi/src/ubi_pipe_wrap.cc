#include "ubi_pipe_wrap.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <chrono>
#include <string>
#include <sstream>
#include <thread>
#if !defined(_WIN32)
#include <unistd.h>
#else
#include <io.h>
#endif

#include <uv.h>

#include "ubi_runtime.h"
#include "ubi_stream_wrap.h"
#include "ubi_tcp_wrap.h"
#include "ubi_udp_wrap.h"

namespace {

constexpr int kPipeSocket = 0;
constexpr int kPipeServer = 1;
constexpr int kPipeIPC = 2;

struct PipeWrap;

struct PipeConnectReqWrap {
  uv_connect_t req{};
  napi_env env = nullptr;
  napi_ref req_obj_ref = nullptr;
  PipeWrap* pipe = nullptr;
};

struct PipeWriteReqWrap {
  uv_write_t req{};
  napi_env env = nullptr;
  napi_ref req_obj_ref = nullptr;
  uv_buf_t* bufs = nullptr;
  uint32_t nbufs = 0;
};

struct PipeShutdownReqWrap {
  uv_shutdown_t req{};
  napi_env env = nullptr;
  napi_ref req_obj_ref = nullptr;
  PipeWrap* pipe = nullptr;
};

struct PipeWrap {
  napi_env env = nullptr;
  napi_ref wrapper_ref = nullptr;
  napi_ref close_cb_ref = nullptr;
  uv_pipe_t handle{};
  int opened_fd = -1;
  bool closed = false;
  bool finalized = false;
  bool delete_on_close = false;
  uint64_t bytes_read = 0;
  uint64_t bytes_written = 0;
  int64_t async_id = 0;
};

napi_ref g_pipe_ctor_ref = nullptr;
int64_t g_next_pipe_async_id = 100000;

int WriteAllToFd(int fd, const char* data, size_t length) {
  if (data == nullptr && length != 0) return UV_EINVAL;
  size_t offset = 0;
  while (offset < length) {
#if defined(_WIN32)
    const int written = _write(fd, data + offset, static_cast<unsigned int>(length - offset));
    if (written < 0) {
      return uv_translate_sys_error(errno);
    }
#else
    const ssize_t written = ::write(fd, data + offset, length - offset);
    if (written < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      return uv_translate_sys_error(errno);
    }
#endif
    if (written == 0) return UV_EIO;
    offset += static_cast<size_t>(written);
  }
  return 0;
}

napi_value GetRefValue(napi_env env, napi_ref ref) {
  if (ref == nullptr) return nullptr;
  napi_value v = nullptr;
  if (napi_get_reference_value(env, ref, &v) != napi_ok) return nullptr;
  return v;
}

void SetReqError(napi_env env, napi_value req_obj, int status) {
  if (req_obj == nullptr || status >= 0) return;
  const char* err = uv_err_name(status);
  napi_value err_v = nullptr;
  napi_create_string_utf8(env, err != nullptr ? err : "UV_ERROR", NAPI_AUTO_LENGTH, &err_v);
  if (err_v != nullptr) napi_set_named_property(env, req_obj, "error", err_v);
}

int32_t* StreamState() { return UbiGetStreamBaseState(); }
void SetState(int idx, int32_t value) {
  int32_t* s = StreamState();
  if (s) s[idx] = value;
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

napi_value BufferFromWithEncoding(napi_env env, napi_value value, napi_value encoding) {
  if (env == nullptr || value == nullptr) return value;

  napi_value global = nullptr;
  napi_value buffer_ctor = nullptr;
  napi_value from_fn = nullptr;
  napi_valuetype type = napi_undefined;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr ||
      napi_get_named_property(env, global, "Buffer", &buffer_ctor) != napi_ok ||
      buffer_ctor == nullptr ||
      napi_get_named_property(env, buffer_ctor, "from", &from_fn) != napi_ok ||
      from_fn == nullptr ||
      napi_typeof(env, from_fn, &type) != napi_ok ||
      type != napi_function) {
    return value;
  }

  napi_value argv[2] = {value, nullptr};
  size_t argc = 1;
  if (encoding != nullptr) {
    napi_valuetype enc_type = napi_undefined;
    if (napi_typeof(env, encoding, &enc_type) == napi_ok && enc_type != napi_undefined) {
      argv[1] = encoding;
      argc = 2;
    }
  }

  napi_value out = nullptr;
  if (napi_call_function(env, buffer_ctor, from_fn, argc, argv, &out) != napi_ok || out == nullptr) {
    return value;
  }
  return out;
}

void FreeWriteReq(PipeWriteReqWrap* wr) {
  if (wr == nullptr) return;
  if (wr->bufs != nullptr) {
    for (uint32_t i = 0; i < wr->nbufs; ++i) {
      if (wr->bufs[i].base != nullptr) free(wr->bufs[i].base);
    }
    delete[] wr->bufs;
    wr->bufs = nullptr;
  }
  if (wr->req_obj_ref != nullptr) {
    napi_delete_reference(wr->env, wr->req_obj_ref);
    wr->req_obj_ref = nullptr;
  }
  delete wr;
}

void InvokeReqOnComplete(napi_env env, napi_value req_obj, int status, napi_value* argv, size_t argc) {
  if (req_obj == nullptr) return;
  SetReqError(env, req_obj, status);
  napi_value oncomplete = nullptr;
  if (napi_get_named_property(env, req_obj, "oncomplete", &oncomplete) != napi_ok || oncomplete == nullptr) return;
  napi_valuetype t = napi_undefined;
  napi_typeof(env, oncomplete, &t);
  if (t != napi_function) return;
  napi_value ignored = nullptr;
  UbiMakeCallback(env, req_obj, oncomplete, argc, argv, &ignored);
}

void OnWriteDone(uv_write_t* req, int status) {
  auto* wr = static_cast<PipeWriteReqWrap*>(req->data);
  if (wr == nullptr) return;
  napi_value req_obj = GetRefValue(wr->env, wr->req_obj_ref);
  napi_value argv[1] = {MakeInt32(wr->env, status)};
  InvokeReqOnComplete(wr->env, req_obj, status, argv, 1);
  FreeWriteReq(wr);
}

void OnShutdownDone(uv_shutdown_t* req, int status) {
  auto* sr = static_cast<PipeShutdownReqWrap*>(req->data);
  if (sr == nullptr) return;
  napi_value req_obj = GetRefValue(sr->env, sr->req_obj_ref);
  napi_value pipe_obj = sr->pipe ? GetRefValue(sr->env, sr->pipe->wrapper_ref) : nullptr;
  napi_value argv[3] = {MakeInt32(sr->env, status), pipe_obj, req_obj};
  InvokeReqOnComplete(sr->env, req_obj, status, argv, 3);
  if (sr->req_obj_ref) napi_delete_reference(sr->env, sr->req_obj_ref);
  delete sr;
}

void OnConnectDone(uv_connect_t* req, int status) {
  auto* cr = static_cast<PipeConnectReqWrap*>(req->data);
  if (cr == nullptr) return;
  napi_value req_obj = GetRefValue(cr->env, cr->req_obj_ref);
  napi_value pipe_obj = cr->pipe ? GetRefValue(cr->env, cr->pipe->wrapper_ref) : nullptr;
  napi_value argv[5] = {
      MakeInt32(cr->env, status),
      pipe_obj,
      req_obj,
      MakeBool(cr->env, true),
      MakeBool(cr->env, true),
  };
  InvokeReqOnComplete(cr->env, req_obj, status, argv, 5);
  if (cr->req_obj_ref) napi_delete_reference(cr->env, cr->req_obj_ref);
  delete cr;
}

void OnAlloc(uv_handle_t* /*h*/, size_t suggested_size, uv_buf_t* buf) {
  char* base = static_cast<char*>(malloc(suggested_size));
  *buf = uv_buf_init(base, static_cast<unsigned int>(suggested_size));
}

napi_value AcceptPendingHandleForIpc(napi_env env, PipeWrap* wrap);

void OnRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
  auto* wrap = static_cast<PipeWrap*>(stream->data);
  if (wrap == nullptr) {
    if (buf && buf->base) free(buf->base);
    return;
  }
  SetState(kUbiReadBytesOrError, static_cast<int32_t>(nread));
  SetState(kUbiArrayBufferOffset, 0);
  if (nread > 0) wrap->bytes_read += static_cast<uint64_t>(nread);

  napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
  napi_value onread = nullptr;
  if (self != nullptr && napi_get_named_property(wrap->env, self, "onread", &onread) == napi_ok) {
    napi_value pending_handle = AcceptPendingHandleForIpc(wrap->env, wrap);
    if (pending_handle != nullptr) {
      napi_valuetype pending_type = napi_undefined;
      if (napi_typeof(wrap->env, pending_handle, &pending_type) == napi_ok &&
          pending_type != napi_undefined) {
        napi_set_named_property(wrap->env, self, "pendingHandle", pending_handle);
      }
    }
    napi_valuetype t = napi_undefined;
    napi_typeof(wrap->env, onread, &t);
    if (t == napi_function) {
      napi_value argv[1] = {nullptr};
      if (nread > 0 && buf && buf->base) {
        void* out = nullptr;
        napi_value ab = nullptr;
        if (napi_create_arraybuffer(wrap->env, nread, &out, &ab) == napi_ok && out && ab) {
          memcpy(out, buf->base, nread);
          argv[0] = ab;
        }
      } else {
        napi_get_undefined(wrap->env, &argv[0]);
      }
      napi_value ignored = nullptr;
      UbiMakeCallback(wrap->env, self, onread, 1, argv, &ignored);
      (void)UbiHandlePendingExceptionNow(wrap->env, nullptr);
    }
  }
  if (buf && buf->base) free(buf->base);
}

void OnClosed(uv_handle_t* h) {
  auto* wrap = static_cast<PipeWrap*>(h->data);
  if (wrap == nullptr) return;
  wrap->closed = true;
  if (!wrap->finalized && wrap->close_cb_ref) {
    napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
    napi_value cb = GetRefValue(wrap->env, wrap->close_cb_ref);
    if (cb) {
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

void PipeFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* wrap = static_cast<PipeWrap*>(data);
  if (!wrap) return;
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

napi_value PipeCtor(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  auto* wrap = new PipeWrap();
  wrap->env = env;
  wrap->async_id = g_next_pipe_async_id++;
  int ipc = 0;
  if (argc >= 1 && argv[0] != nullptr) {
    int32_t type = 0;
    if (napi_get_value_int32(env, argv[0], &type) == napi_ok && type == kPipeIPC) {
      ipc = 1;
    }
  }
  uv_pipe_init(uv_default_loop(), &wrap->handle, ipc);
  wrap->handle.data = wrap;
  napi_wrap(env, self, wrap, PipeFinalize, nullptr, &wrap->wrapper_ref);
  napi_set_named_property(env, self, "isStreamBase", MakeBool(env, true));
  napi_set_named_property(env, self, "reading", MakeBool(env, false));
  return self;
}

napi_value PipeOpen(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (!wrap || argc < 1) return MakeInt32(env, UV_EINVAL);
  int32_t fd = -1;
  napi_get_value_int32(env, argv[0], &fd);
  wrap->opened_fd = fd;
  int rc = uv_pipe_open(&wrap->handle, static_cast<uv_file>(fd));
  return MakeInt32(env, rc);
}

napi_value PipeBind(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (!wrap || argc < 1) return MakeInt32(env, UV_EINVAL);
  std::string path = ValueToUtf8(env, argv[0]);
  int rc = uv_pipe_bind2(&wrap->handle,
                         path.c_str(),
                         path.size(),
                         UV_PIPE_NO_TRUNCATE);
  return MakeInt32(env, rc);
}

void OnConnection(uv_stream_t* server, int status) {
  auto* server_wrap = static_cast<PipeWrap*>(server->data);
  if (!server_wrap) return;
  napi_env env = server_wrap->env;
  napi_value server_obj = GetRefValue(env, server_wrap->wrapper_ref);
  napi_value onconnection = nullptr;
  if (!server_obj || napi_get_named_property(env, server_obj, "onconnection", &onconnection) != napi_ok) return;
  napi_valuetype t = napi_undefined;
  napi_typeof(env, onconnection, &t);
  if (t != napi_function) return;

  napi_value argv[2] = {MakeInt32(env, status), nullptr};
  if (status == 0) {
    napi_value ctor = GetRefValue(env, g_pipe_ctor_ref);
    napi_value arg0 = nullptr;
    napi_create_int32(env, kPipeSocket, &arg0);
    napi_value client_obj = nullptr;
    napi_new_instance(env, ctor, 1, &arg0, &client_obj);
    PipeWrap* client_wrap = nullptr;
    napi_unwrap(env, client_obj, reinterpret_cast<void**>(&client_wrap));
    int rc = uv_accept(server, reinterpret_cast<uv_stream_t*>(&client_wrap->handle));
    argv[0] = MakeInt32(env, rc);
    argv[1] = client_obj;
  }
  napi_value ignored = nullptr;
  UbiMakeCallback(env, server_obj, onconnection, 2, argv, &ignored);
}

napi_value PipeListen(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (!wrap || argc < 1) return MakeInt32(env, UV_EINVAL);
  int32_t backlog = 511;
  napi_get_value_int32(env, argv[0], &backlog);
  int rc = uv_listen(reinterpret_cast<uv_stream_t*>(&wrap->handle), backlog, OnConnection);
  return MakeInt32(env, rc);
}

napi_value PipeConnect(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (!wrap || argc < 2) return MakeInt32(env, UV_EINVAL);
  auto* cr = new PipeConnectReqWrap();
  cr->env = env;
  cr->pipe = wrap;
  cr->req.data = cr;
  napi_create_reference(env, argv[0], 1, &cr->req_obj_ref);
  std::string path = ValueToUtf8(env, argv[1]);
  int rc = uv_pipe_connect2(&cr->req,
                            &wrap->handle,
                            path.c_str(),
                            path.size(),
                            UV_PIPE_NO_TRUNCATE,
                            OnConnectDone);
  if (rc != 0) {
    if (cr->req_obj_ref) napi_delete_reference(env, cr->req_obj_ref);
    delete cr;
  }
  return MakeInt32(env, rc);
}

napi_value PipeReadStart(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (!wrap) return MakeInt32(env, UV_EINVAL);
  int rc = uv_read_start(reinterpret_cast<uv_stream_t*>(&wrap->handle), OnAlloc, OnRead);
  napi_set_named_property(env, self, "reading", MakeBool(env, rc == 0));
  return MakeInt32(env, rc);
}

napi_value PipeReadStop(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (!wrap) return MakeInt32(env, UV_EINVAL);
  int rc = uv_read_stop(reinterpret_cast<uv_stream_t*>(&wrap->handle));
  napi_set_named_property(env, self, "reading", MakeBool(env, false));
  return MakeInt32(env, rc);
}

napi_value PipeWriteBuffer(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (!wrap || argc < 2) return MakeInt32(env, UV_EINVAL);

  size_t length = 0;
  void* raw = nullptr;
  std::string temp_utf8;
  bool is_buffer = false;
  napi_is_buffer(env, argv[1], &is_buffer);
  if (is_buffer) {
    napi_get_buffer_info(env, argv[1], &raw, &length);
  }
  bool is_typed = false;
  if (!is_buffer) napi_is_typedarray(env, argv[1], &is_typed);
  if (is_typed) {
    napi_typedarray_type tt;
    napi_value ab;
    size_t off;
    napi_get_typedarray_info(env, argv[1], &tt, &length, &raw, &ab, &off);
  } else if (!is_buffer) {
    temp_utf8 = ValueToUtf8(env, argv[1]);
    length = temp_utf8.size();
    raw = const_cast<char*>(temp_utf8.data());
  }

  uv_stream_t* send_handle = nullptr;
  if (argc >= 3 && argv[2] != nullptr) {
    send_handle = UbiTcpWrapGetStream(env, argv[2]);
    if (send_handle == nullptr) send_handle = UbiPipeWrapGetStream(env, argv[2]);
    if (send_handle == nullptr) {
      uv_handle_t* udp_handle = UbiUdpWrapGetHandle(env, argv[2]);
      send_handle = reinterpret_cast<uv_stream_t*>(udp_handle);
    }
  }

  const bool is_stdio_fd = (wrap->opened_fd == 1 || wrap->opened_fd == 2);
  if (send_handle == nullptr && is_stdio_fd) {
    const char* bytes = static_cast<const char*>(raw);
    const int rc = WriteAllToFd(wrap->opened_fd, bytes, length);
    if (rc == 0) {
      wrap->bytes_written += length;
      SetState(kUbiBytesWritten, static_cast<int32_t>(length));
      SetState(kUbiLastWriteWasAsync, 0);
    } else {
      SetState(kUbiBytesWritten, 0);
      SetState(kUbiLastWriteWasAsync, 0);
      SetReqError(env, argv[0], rc);
    }
    return MakeInt32(env, rc);
  }

  auto* wr = new PipeWriteReqWrap();
  wr->env = env;
  napi_create_reference(env, argv[0], 1, &wr->req_obj_ref);
  wr->nbufs = 1;
  wr->bufs = new uv_buf_t[1];
  char* copy = static_cast<char*>(malloc(length));
  if (length > 0 && raw != nullptr) memcpy(copy, raw, length);
  wr->bufs[0] = uv_buf_init(copy, static_cast<unsigned int>(length));
  wr->req.data = wr;
  wrap->bytes_written += length;
  SetState(kUbiBytesWritten, static_cast<int32_t>(length));
  SetState(kUbiLastWriteWasAsync, 1);
  int rc;
  if (send_handle != nullptr) {
    rc = uv_write2(&wr->req,
                   reinterpret_cast<uv_stream_t*>(&wrap->handle),
                   wr->bufs,
                   wr->nbufs,
                   send_handle,
                   OnWriteDone);
  } else {
    rc = uv_write(&wr->req,
                  reinterpret_cast<uv_stream_t*>(&wrap->handle),
                  wr->bufs,
                  wr->nbufs,
                  OnWriteDone);
  }
  if (rc != 0) {
    napi_value req_obj = GetRefValue(env, wr->req_obj_ref);
    SetReqError(env, req_obj, rc);
    FreeWriteReq(wr);
  }
  return MakeInt32(env, rc);
}

napi_value PipeWriteString(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr};
  napi_value self = nullptr;
  void* data = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, &data);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (!wrap || argc < 2) return MakeInt32(env, UV_EINVAL);

  napi_value payload = argv[1];
  const char* encoding_name = static_cast<const char*>(data);
  if (encoding_name != nullptr && payload != nullptr) {
    napi_value encoding = nullptr;
    if (napi_create_string_utf8(env, encoding_name, NAPI_AUTO_LENGTH, &encoding) == napi_ok &&
        encoding != nullptr) {
      payload = BufferFromWithEncoding(env, payload, encoding);
    }
  }

  size_t length = 0;
  void* raw = nullptr;
  std::string temp_utf8;
  bool is_buffer = false;
  napi_is_buffer(env, payload, &is_buffer);
  if (is_buffer) {
    napi_get_buffer_info(env, payload, &raw, &length);
  }
  bool is_typed = false;
  if (!is_buffer) napi_is_typedarray(env, payload, &is_typed);
  if (is_typed) {
    napi_typedarray_type tt;
    napi_value ab;
    size_t off;
    napi_get_typedarray_info(env, payload, &tt, &length, &raw, &ab, &off);
  } else if (!is_buffer) {
    temp_utf8 = ValueToUtf8(env, payload);
    length = temp_utf8.size();
    raw = const_cast<char*>(temp_utf8.data());
  }

  uv_stream_t* send_handle = nullptr;
  if (argc >= 3 && argv[2] != nullptr) {
    send_handle = UbiTcpWrapGetStream(env, argv[2]);
    if (send_handle == nullptr) send_handle = UbiPipeWrapGetStream(env, argv[2]);
    if (send_handle == nullptr) {
      uv_handle_t* udp_handle = UbiUdpWrapGetHandle(env, argv[2]);
      send_handle = reinterpret_cast<uv_stream_t*>(udp_handle);
    }
  }

  const bool is_stdio_fd = (wrap->opened_fd == 1 || wrap->opened_fd == 2);
  if (send_handle == nullptr && is_stdio_fd) {
    const char* bytes = static_cast<const char*>(raw);
    const int rc = WriteAllToFd(wrap->opened_fd, bytes, length);
    if (rc == 0) {
      wrap->bytes_written += length;
      SetState(kUbiBytesWritten, static_cast<int32_t>(length));
      SetState(kUbiLastWriteWasAsync, 0);
    } else {
      SetState(kUbiBytesWritten, 0);
      SetState(kUbiLastWriteWasAsync, 0);
      SetReqError(env, argv[0], rc);
    }
    return MakeInt32(env, rc);
  }

  auto* wr = new PipeWriteReqWrap();
  wr->env = env;
  napi_create_reference(env, argv[0], 1, &wr->req_obj_ref);
  wr->nbufs = 1;
  wr->bufs = new uv_buf_t[1];
  char* copy = static_cast<char*>(malloc(length));
  if (length > 0 && raw != nullptr) memcpy(copy, raw, length);
  wr->bufs[0] = uv_buf_init(copy, static_cast<unsigned int>(length));
  wr->req.data = wr;
  wrap->bytes_written += length;
  SetState(kUbiBytesWritten, static_cast<int32_t>(length));
  SetState(kUbiLastWriteWasAsync, 1);
  int rc;
  if (send_handle != nullptr) {
    rc = uv_write2(&wr->req,
                   reinterpret_cast<uv_stream_t*>(&wrap->handle),
                   wr->bufs,
                   wr->nbufs,
                   send_handle,
                   OnWriteDone);
  } else {
    rc = uv_write(&wr->req,
                  reinterpret_cast<uv_stream_t*>(&wrap->handle),
                  wr->bufs,
                  wr->nbufs,
                  OnWriteDone);
  }
  if (rc != 0) {
    napi_value req_obj = GetRefValue(env, wr->req_obj_ref);
    SetReqError(env, req_obj, rc);
    FreeWriteReq(wr);
  }
  return MakeInt32(env, rc);
}

napi_value PipeWritev(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (!wrap || argc < 2) return MakeInt32(env, UV_EINVAL);

  const napi_value req_obj = argv[0];
  const napi_value chunks = argv[1];
  bool all_buffers = false;
  if (argc > 2 && argv[2] != nullptr) napi_get_value_bool(env, argv[2], &all_buffers);

  uint32_t raw_len = 0;
  napi_get_array_length(env, chunks, &raw_len);
  const uint32_t nbufs = all_buffers ? raw_len : (raw_len / 2);
  if (nbufs == 0) {
    SetState(kUbiBytesWritten, 0);
    SetState(kUbiLastWriteWasAsync, 0);
    return MakeInt32(env, 0);
  }

  auto* wr = new PipeWriteReqWrap();
  wr->env = env;
  napi_create_reference(env, req_obj, 1, &wr->req_obj_ref);
  wr->nbufs = nbufs;
  wr->bufs = new uv_buf_t[nbufs];
  size_t total = 0;

  for (uint32_t i = 0; i < nbufs; ++i) {
    napi_value chunk = nullptr;
    napi_get_element(env, chunks, all_buffers ? i : (i * 2), &chunk);
    if (!all_buffers) {
      bool is_buffer = false;
      napi_is_buffer(env, chunk, &is_buffer);
      bool is_typed = false;
      if (!is_buffer) napi_is_typedarray(env, chunk, &is_typed);
      if (!is_buffer && !is_typed) {
        napi_value encoding = nullptr;
        napi_get_element(env, chunks, i * 2 + 1, &encoding);
        chunk = BufferFromWithEncoding(env, chunk, encoding);
      }
    }
    bool is_buffer = false;
    napi_is_buffer(env, chunk, &is_buffer);
    bool is_typed = false;
    if (!is_buffer) napi_is_typedarray(env, chunk, &is_typed);
    size_t length = 0;
    void* raw = nullptr;
    std::string temp_utf8;
    if (is_buffer) {
      napi_get_buffer_info(env, chunk, &raw, &length);
    } else if (is_typed) {
      napi_typedarray_type tt;
      napi_value ab = nullptr;
      size_t off = 0;
      if (napi_get_typedarray_info(env, chunk, &tt, &length, &raw, &ab, &off) != napi_ok) {
        raw = nullptr;
        length = 0;
      }
    } else {
      temp_utf8 = ValueToUtf8(env, chunk);
      raw = const_cast<char*>(temp_utf8.data());
      length = temp_utf8.size();
    }
    char* copy = static_cast<char*>(malloc(length));
    if (length > 0 && raw != nullptr) memcpy(copy, raw, length);
    wr->bufs[i] = uv_buf_init(copy, static_cast<unsigned int>(length));
    total += length;
  }

  wr->req.data = wr;
  const bool is_stdio_fd = (wrap->opened_fd == 1 || wrap->opened_fd == 2);
  if (is_stdio_fd) {
    int rc = 0;
    size_t written_total = 0;
    for (uint32_t i = 0; i < wr->nbufs; ++i) {
      const uv_buf_t& b = wr->bufs[i];
      const int part = WriteAllToFd(wrap->opened_fd, b.base, b.len);
      if (part != 0) {
        rc = part;
        break;
      }
      written_total += b.len;
    }
    if (rc == 0) {
      wrap->bytes_written += written_total;
      SetState(kUbiBytesWritten, static_cast<int32_t>(written_total));
      SetState(kUbiLastWriteWasAsync, 0);
    } else {
      SetState(kUbiBytesWritten, static_cast<int32_t>(written_total));
      SetState(kUbiLastWriteWasAsync, 0);
      SetReqError(env, req_obj, rc);
    }
    FreeWriteReq(wr);
    return MakeInt32(env, rc);
  }

  wrap->bytes_written += total;
  SetState(kUbiBytesWritten, static_cast<int32_t>(total));
  SetState(kUbiLastWriteWasAsync, 1);

  int rc = uv_write(&wr->req,
                    reinterpret_cast<uv_stream_t*>(&wrap->handle),
                    wr->bufs,
                    wr->nbufs,
                    OnWriteDone);
  if (rc != 0) {
    SetReqError(env, req_obj, rc);
    FreeWriteReq(wr);
  }
  return MakeInt32(env, rc);
}

napi_value PipeShutdown(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (!wrap || argc < 1) return MakeInt32(env, UV_EINVAL);
  auto* sr = new PipeShutdownReqWrap();
  sr->env = env;
  sr->pipe = wrap;
  sr->req.data = sr;
  napi_create_reference(env, argv[0], 1, &sr->req_obj_ref);
  int rc = uv_shutdown(&sr->req, reinterpret_cast<uv_stream_t*>(&wrap->handle), OnShutdownDone);
  if (rc != 0) {
    napi_value req_obj = GetRefValue(env, sr->req_obj_ref);
    SetReqError(env, req_obj, rc);
    napi_delete_reference(env, sr->req_obj_ref);
    delete sr;
  }
  return MakeInt32(env, rc);
}

napi_value PipeClose(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (!wrap) {
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

napi_value PipeSetPendingInstances(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (!wrap || argc < 1) return MakeInt32(env, UV_EINVAL);
  int32_t n = 0;
  napi_get_value_int32(env, argv[0], &n);
  uv_pipe_pending_instances(&wrap->handle, n);
  return MakeInt32(env, 0);
}

napi_value PipeFchmod(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (!wrap || argc < 1) return MakeInt32(env, UV_EINVAL);
#ifdef UV_VERSION_MAJOR
  int32_t mode = 0;
  napi_get_value_int32(env, argv[0], &mode);
  int rc = uv_pipe_chmod(&wrap->handle, mode);
  return MakeInt32(env, rc);
#else
  return MakeInt32(env, UV_ENOTSUP);
#endif
}

napi_value PipeUseUserBuffer(napi_env env, napi_callback_info info) {
  (void)info;
  napi_value u = nullptr;
  napi_get_undefined(env, &u);
  return u;
}

napi_value PipeRef(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap) uv_ref(reinterpret_cast<uv_handle_t*>(&wrap->handle));
  napi_value u = nullptr;
  napi_get_undefined(env, &u);
  return u;
}

napi_value PipeUnref(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap) uv_unref(reinterpret_cast<uv_handle_t*>(&wrap->handle));
  napi_value u = nullptr;
  napi_get_undefined(env, &u);
  return u;
}

napi_value PipeGetAsyncId(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  napi_value out = nullptr;
  napi_create_int64(env, wrap ? wrap->async_id : -1, &out);
  return out;
}

napi_value PipeGetProviderType(napi_env env, napi_callback_info info) {
  (void)info;
  return MakeInt32(env, kPipeSocket);
}

napi_value PipeAsyncReset(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap) wrap->async_id = g_next_pipe_async_id++;
  napi_value u = nullptr;
  napi_get_undefined(env, &u);
  return u;
}

napi_value PipeGetSockName(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (!wrap || argc < 1) return MakeInt32(env, UV_EINVAL);
  size_t len = 0;
  uv_pipe_getsockname(&wrap->handle, nullptr, &len);
  std::string name(len, '\0');
  int rc = uv_pipe_getsockname(&wrap->handle, name.data(), &len);
  if (rc == 0) {
    name.resize(len);
    napi_value s = nullptr;
    napi_create_string_utf8(env, name.c_str(), name.size(), &s);
    if (s) napi_set_named_property(env, argv[0], "address", s);
  }
  return MakeInt32(env, rc);
}

napi_value PipeGetPeerName(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (!wrap || argc < 1) return MakeInt32(env, UV_EINVAL);
  size_t len = 0;
  uv_pipe_getpeername(&wrap->handle, nullptr, &len);
  std::string name(len, '\0');
  int rc = uv_pipe_getpeername(&wrap->handle, name.data(), &len);
  if (rc == 0) {
    name.resize(len);
    napi_value s = nullptr;
    napi_create_string_utf8(env, name.c_str(), name.size(), &s);
    if (s) napi_set_named_property(env, argv[0], "address", s);
  }
  return MakeInt32(env, rc);
}

napi_value AcceptPendingHandleForIpc(napi_env env, PipeWrap* wrap) {
  if (env == nullptr || wrap == nullptr) {
    napi_value u = nullptr;
    napi_get_undefined(env, &u);
    return u;
  }

  int count = uv_pipe_pending_count(&wrap->handle);
  if (count <= 0) {
    napi_value u = nullptr;
    napi_get_undefined(env, &u);
    return u;
  }

  uv_handle_type pending_type = uv_pipe_pending_type(&wrap->handle);
  napi_value handle_obj = nullptr;
  uv_stream_t* accept_target = nullptr;

  if (pending_type == UV_TCP) {
    napi_value tcp_ctor = UbiGetTcpWrapConstructor(env);
    napi_value arg = nullptr;
    if (tcp_ctor == nullptr ||
        napi_create_int32(env, 0, &arg) != napi_ok ||
        arg == nullptr ||
        napi_new_instance(env, tcp_ctor, 1, &arg, &handle_obj) != napi_ok ||
        handle_obj == nullptr) {
      napi_value u = nullptr;
      napi_get_undefined(env, &u);
      return u;
    }
    accept_target = UbiTcpWrapGetStream(env, handle_obj);
  } else if (pending_type == UV_NAMED_PIPE) {
    napi_value ctor = GetRefValue(env, g_pipe_ctor_ref);
    napi_value arg = nullptr;
    if (ctor == nullptr ||
        napi_create_int32(env, kPipeSocket, &arg) != napi_ok ||
        arg == nullptr ||
        napi_new_instance(env, ctor, 1, &arg, &handle_obj) != napi_ok ||
        handle_obj == nullptr) {
      napi_value u = nullptr;
      napi_get_undefined(env, &u);
      return u;
    }
    accept_target = UbiPipeWrapGetStream(env, handle_obj);
  } else if (pending_type == UV_UDP) {
    napi_value udp_ctor = UbiGetUdpWrapConstructor(env);
    if (udp_ctor == nullptr ||
        napi_new_instance(env, udp_ctor, 0, nullptr, &handle_obj) != napi_ok ||
        handle_obj == nullptr) {
      napi_value u = nullptr;
      napi_get_undefined(env, &u);
      return u;
    }
    uv_handle_t* udp_handle = UbiUdpWrapGetHandle(env, handle_obj);
    accept_target = reinterpret_cast<uv_stream_t*>(udp_handle);
  } else {
    napi_value u = nullptr;
    napi_get_undefined(env, &u);
    return u;
  }

  if (accept_target == nullptr) {
    napi_value u = nullptr;
    napi_get_undefined(env, &u);
    return u;
  }

  int rc = uv_accept(reinterpret_cast<uv_stream_t*>(&wrap->handle), accept_target);
  if (rc != 0) {
    napi_value u = nullptr;
    napi_get_undefined(env, &u);
    return u;
  }
  return handle_obj;
}

napi_value PipeAcceptPendingHandle(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  return AcceptPendingHandleForIpc(env, wrap);
}

napi_value PipeBytesReadGetter(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  napi_value out = nullptr;
  napi_create_double(env, static_cast<double>(wrap ? wrap->bytes_read : 0), &out);
  return out;
}

napi_value PipeBytesWrittenGetter(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  napi_value out = nullptr;
  napi_create_double(env, static_cast<double>(wrap ? wrap->bytes_written : 0), &out);
  return out;
}

napi_value PipeWriteQueueSizeGetter(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  const size_t qsize =
      (wrap != nullptr) ? uv_stream_get_write_queue_size(reinterpret_cast<uv_stream_t*>(&wrap->handle)) : 0;
  napi_value out = nullptr;
  napi_create_double(env, static_cast<double>(qsize), &out);
  return out;
}

napi_value PipeFdGetter(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  PipeWrap* wrap = nullptr;
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

napi_value PipeConnectWrapCtor(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  return self;
}

void SetNamedU32(napi_env env, napi_value obj, const char* key, uint32_t value) {
  napi_value v = nullptr;
  napi_create_uint32(env, value, &v);
  if (v) napi_set_named_property(env, obj, key, v);
}

}  // namespace

uv_stream_t* UbiPipeWrapGetStream(napi_env env, napi_value value) {
  if (env == nullptr || value == nullptr) return nullptr;
  napi_valuetype t = napi_undefined;
  if (napi_typeof(env, value, &t) != napi_ok || t != napi_object) return nullptr;
  PipeWrap* wrap = nullptr;
  if (napi_unwrap(env, value, reinterpret_cast<void**>(&wrap)) != napi_ok || wrap == nullptr) return nullptr;
  return reinterpret_cast<uv_stream_t*>(&wrap->handle);
}

napi_value UbiInstallPipeWrapBinding(napi_env env) {
  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) return nullptr;

  napi_property_descriptor pipe_props[] = {
      {"open", nullptr, PipeOpen, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"bind", nullptr, PipeBind, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"listen", nullptr, PipeListen, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"connect", nullptr, PipeConnect, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"close", nullptr, PipeClose, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"readStart", nullptr, PipeReadStart, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"readStop", nullptr, PipeReadStop, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"writeBuffer", nullptr, PipeWriteBuffer, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"writev", nullptr, PipeWritev, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"writeLatin1String", nullptr, PipeWriteString, nullptr, nullptr, nullptr, napi_default_method,
       const_cast<char*>("latin1")},
      {"writeUtf8String", nullptr, PipeWriteString, nullptr, nullptr, nullptr, napi_default_method,
       const_cast<char*>("utf8")},
      {"writeAsciiString", nullptr, PipeWriteString, nullptr, nullptr, nullptr, napi_default_method,
       const_cast<char*>("ascii")},
      {"writeUcs2String", nullptr, PipeWriteString, nullptr, nullptr, nullptr, napi_default_method,
       const_cast<char*>("ucs2")},
      {"shutdown", nullptr, PipeShutdown, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"setPendingInstances", nullptr, PipeSetPendingInstances, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"fchmod", nullptr, PipeFchmod, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"useUserBuffer", nullptr, PipeUseUserBuffer, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"acceptPendingHandle", nullptr, PipeAcceptPendingHandle, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"ref", nullptr, PipeRef, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"unref", nullptr, PipeUnref, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getAsyncId", nullptr, PipeGetAsyncId, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getProviderType", nullptr, PipeGetProviderType, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"asyncReset", nullptr, PipeAsyncReset, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"bytesRead", nullptr, nullptr, PipeBytesReadGetter, nullptr, nullptr, napi_default, nullptr},
      {"bytesWritten", nullptr, nullptr, PipeBytesWrittenGetter, nullptr, nullptr, napi_default, nullptr},
      {"writeQueueSize", nullptr, nullptr, PipeWriteQueueSizeGetter, nullptr, nullptr, napi_default, nullptr},
      {"fd", nullptr, nullptr, PipeFdGetter, nullptr, nullptr, napi_default, nullptr},
  };

  napi_value pipe_ctor = nullptr;
  if (napi_define_class(env,
                        "Pipe",
                        NAPI_AUTO_LENGTH,
                        PipeCtor,
                        nullptr,
                        sizeof(pipe_props) / sizeof(pipe_props[0]),
                        pipe_props,
                        &pipe_ctor) != napi_ok ||
      pipe_ctor == nullptr) {
    return nullptr;
  }
  if (g_pipe_ctor_ref != nullptr) napi_delete_reference(env, g_pipe_ctor_ref);
  napi_create_reference(env, pipe_ctor, 1, &g_pipe_ctor_ref);

  napi_value connect_wrap_ctor = nullptr;
  if (napi_define_class(env,
                        "PipeConnectWrap",
                        NAPI_AUTO_LENGTH,
                        PipeConnectWrapCtor,
                        nullptr,
                        0,
                        nullptr,
                        &connect_wrap_ctor) != napi_ok ||
      connect_wrap_ctor == nullptr) {
    return nullptr;
  }

  napi_value constants = nullptr;
  napi_create_object(env, &constants);
  SetNamedU32(env, constants, "SOCKET", kPipeSocket);
  SetNamedU32(env, constants, "SERVER", kPipeServer);
  SetNamedU32(env, constants, "IPC", kPipeIPC);
  SetNamedU32(env, constants, "UV_READABLE", static_cast<uint32_t>(UV_READABLE));
  SetNamedU32(env, constants, "UV_WRITABLE", static_cast<uint32_t>(UV_WRITABLE));

  napi_set_named_property(env, binding, "Pipe", pipe_ctor);
  napi_set_named_property(env, binding, "PipeConnectWrap", connect_wrap_ctor);
  napi_set_named_property(env, binding, "constants", constants);

  return binding;
}
