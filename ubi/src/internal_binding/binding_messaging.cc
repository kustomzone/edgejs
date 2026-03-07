#include "internal_binding/dispatch.h"

#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <uv.h>

#include "internal_binding/helpers.h"
#include "unofficial_napi.h"
#include "../ubi_module_loader.h"
#include "ubi_active_resource.h"
#include "ubi_env_loop.h"
#include "ubi_handle_wrap.h"
#include "ubi_runtime.h"

namespace internal_binding {

namespace {

struct QueuedMessage {
  napi_ref payload_ref = nullptr;
  bool is_close = false;
};

struct MessagePortWrap {
  UbiHandleWrap handle_wrap{};
  napi_ref peer_ref = nullptr;
  std::deque<QueuedMessage> queued_messages;
  std::mutex mutex;
  uv_async_t async{};
  bool receiving_messages = false;
  bool close_message_enqueued = false;
};

struct MessagingState {
  napi_ref binding_ref = nullptr;
  napi_ref deserializer_create_object_ref = nullptr;
  napi_ref emit_message_ref = nullptr;
  napi_ref message_port_ctor_ref = nullptr;
  napi_ref no_message_symbol_ref = nullptr;
  napi_ref oninit_symbol_ref = nullptr;
  napi_ref handle_onclose_symbol_ref = nullptr;
};

std::unordered_map<napi_env, MessagingState> g_messaging_states;

napi_value ResolveDOMExceptionValue(napi_env env);
napi_value ResolveEmitMessageValue(napi_env env);

napi_value GetNamed(napi_env env, napi_value obj, const char* key) {
  napi_value out = nullptr;
  if (obj == nullptr || napi_get_named_property(env, obj, key, &out) != napi_ok || out == nullptr) {
    return nullptr;
  }
  return out;
}

bool IsFunction(napi_env env, napi_value value) {
  if (value == nullptr) return false;
  napi_valuetype type = napi_undefined;
  return napi_typeof(env, value, &type) == napi_ok && type == napi_function;
}

bool IsUndefinedValue(napi_env env, napi_value value) {
  if (value == nullptr) return true;
  napi_valuetype type = napi_undefined;
  return napi_typeof(env, value, &type) == napi_ok && type == napi_undefined;
}

bool IsNullOrUndefinedValue(napi_env env, napi_value value) {
  if (value == nullptr) return true;
  napi_valuetype type = napi_undefined;
  return napi_typeof(env, value, &type) == napi_ok &&
         (type == napi_undefined || type == napi_null);
}

std::string ValueToUtf8(napi_env env, napi_value value) {
  if (value == nullptr) return {};
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

void ClearPendingException(napi_env env) {
  bool pending = false;
  if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
    napi_value ignored = nullptr;
    napi_get_and_clear_last_exception(env, &ignored);
  }
}

void DeleteRefIfPresent(napi_env env, napi_ref* ref) {
  if (ref == nullptr || *ref == nullptr) return;
  napi_delete_reference(env, *ref);
  *ref = nullptr;
}

napi_value CreateDataCloneError(napi_env env, const char* message) {
  napi_value msg = nullptr;
  napi_value err = nullptr;
  napi_create_string_utf8(env, message, NAPI_AUTO_LENGTH, &msg);
  napi_create_error(env, nullptr, msg, &err);
  if (err == nullptr) return nullptr;

  napi_value code = nullptr;
  napi_create_int32(env, 25, &code);
  if (code != nullptr) napi_set_named_property(env, err, "code", code);

  napi_value name = nullptr;
  napi_create_string_utf8(env, "DataCloneError", NAPI_AUTO_LENGTH, &name);
  if (name != nullptr) napi_set_named_property(env, err, "name", name);
  return err;
}

napi_value GetRefValue(napi_env env, napi_ref ref) {
  if (ref == nullptr) return nullptr;
  napi_value value = nullptr;
  if (napi_get_reference_value(env, ref, &value) != napi_ok || value == nullptr) return nullptr;
  return value;
}

void SetRefToValue(napi_env env, napi_ref* slot, napi_value value) {
  if (slot == nullptr) return;
  DeleteRefIfPresent(env, slot);
  if (value == nullptr) return;
  napi_create_reference(env, value, 1, slot);
}

napi_value TryRequireModule(napi_env env, const char* module_name) {
  napi_value global = GetGlobal(env);
  if (global == nullptr) return nullptr;
  napi_value require_fn = UbiGetRequireFunction(env);
  if (!IsFunction(env, require_fn)) {
    require_fn = GetNamed(env, global, "require");
  }
  if (!IsFunction(env, require_fn)) return nullptr;

  napi_value module_name_v = nullptr;
  if (napi_create_string_utf8(env, module_name, NAPI_AUTO_LENGTH, &module_name_v) != napi_ok ||
      module_name_v == nullptr) {
    return nullptr;
  }

  napi_value out = nullptr;
  napi_value argv[1] = {module_name_v};
  if (napi_call_function(env, global, require_fn, 1, argv, &out) != napi_ok || out == nullptr) {
    ClearPendingException(env);
    return nullptr;
  }
  return out;
}

napi_value GetUntransferableObjectPrivateSymbol(napi_env env) {
  napi_value global = GetGlobal(env);
  if (global == nullptr) return nullptr;

  napi_value internal_binding = UbiGetInternalBinding(env);
  if (!IsFunction(env, internal_binding)) {
    internal_binding = GetNamed(env, global, "internalBinding");
  }
  if (!IsFunction(env, internal_binding)) return nullptr;

  napi_value util_name = nullptr;
  if (napi_create_string_utf8(env, "util", NAPI_AUTO_LENGTH, &util_name) != napi_ok || util_name == nullptr) {
    return nullptr;
  }

  napi_value util_binding = nullptr;
  napi_value argv[1] = {util_name};
  if (napi_call_function(env, global, internal_binding, 1, argv, &util_binding) != napi_ok || util_binding == nullptr) {
    ClearPendingException(env);
    return nullptr;
  }

  napi_value private_symbols = GetNamed(env, util_binding, "privateSymbols");
  if (private_symbols == nullptr) return nullptr;
  return GetNamed(env, private_symbols, "untransferable_object_private_symbol");
}

bool TransferListContainsMarkedUntransferable(napi_env env, napi_value transfer_list) {
  if (transfer_list == nullptr || IsUndefinedValue(env, transfer_list)) return false;

  bool is_array = false;
  if (napi_is_array(env, transfer_list, &is_array) != napi_ok || !is_array) return false;

  napi_value marker = GetUntransferableObjectPrivateSymbol(env);
  if (marker == nullptr || IsUndefinedValue(env, marker)) return false;

  uint32_t length = 0;
  if (napi_get_array_length(env, transfer_list, &length) != napi_ok || length == 0) return false;

  for (uint32_t i = 0; i < length; ++i) {
    napi_value item = nullptr;
    if (napi_get_element(env, transfer_list, i, &item) != napi_ok || item == nullptr) continue;

    napi_valuetype type = napi_undefined;
    if (napi_typeof(env, item, &type) != napi_ok || (type != napi_object && type != napi_function)) continue;

    bool has_marker = false;
    if (napi_has_property(env, item, marker, &has_marker) == napi_ok && has_marker) {
      napi_value marker_value = nullptr;
      if (napi_get_property(env, item, marker, &marker_value) == napi_ok && !IsUndefinedValue(env, marker_value)) {
        return true;
      }
    }
  }

  return false;
}

napi_value GetNoMessageSymbol(napi_env env) {
  const auto it = g_messaging_states.find(env);
  if (it == g_messaging_states.end()) return nullptr;
  return GetRefValue(env, it->second.no_message_symbol_ref);
}

napi_value GetOnInitSymbol(napi_env env) {
  const auto it = g_messaging_states.find(env);
  if (it == g_messaging_states.end()) return nullptr;
  return GetRefValue(env, it->second.oninit_symbol_ref);
}

napi_value GetHandleOnCloseSymbol(napi_env env) {
  const auto it = g_messaging_states.find(env);
  if (it == g_messaging_states.end()) return nullptr;
  return GetRefValue(env, it->second.handle_onclose_symbol_ref);
}

MessagePortWrap* UnwrapMessagePort(napi_env env, napi_value value) {
  MessagePortWrap* wrap = nullptr;
  if (value == nullptr) return nullptr;
  if (napi_unwrap(env, value, reinterpret_cast<void**>(&wrap)) != napi_ok || wrap == nullptr) return nullptr;
  return wrap;
}

bool MessagePortHasRefActive(void* data) {
  auto* wrap = static_cast<MessagePortWrap*>(data);
  return wrap != nullptr &&
         UbiHandleWrapHasRef(&wrap->handle_wrap, reinterpret_cast<const uv_handle_t*>(&wrap->async));
}

napi_value MessagePortGetActiveOwner(napi_env env, void* data) {
  auto* wrap = static_cast<MessagePortWrap*>(data);
  return wrap != nullptr ? UbiHandleWrapGetActiveOwner(env, wrap->handle_wrap.wrapper_ref) : nullptr;
}

void DeleteQueuedMessages(napi_env env, MessagePortWrap* wrap) {
  if (wrap == nullptr) return;
  std::deque<QueuedMessage> queued;
  {
    std::lock_guard<std::mutex> lock(wrap->mutex);
    queued.swap(wrap->queued_messages);
    wrap->close_message_enqueued = false;
  }
  for (auto& entry : queued) {
    DeleteRefIfPresent(env, &entry.payload_ref);
  }
}

void TriggerPortAsync(MessagePortWrap* wrap) {
  if (wrap == nullptr || wrap->handle_wrap.state != kUbiHandleInitialized ||
      uv_is_closing(reinterpret_cast<uv_handle_t*>(&wrap->async))) {
    return;
  }
  uv_async_send(&wrap->async);
}

napi_value CreateStructuredCloneOptions(napi_env env, napi_value value) {
  if (value == nullptr || IsUndefinedValue(env, value) || IsNullOrUndefinedValue(env, value)) return nullptr;
  bool is_array = false;
  if (napi_is_array(env, value, &is_array) == napi_ok && is_array) {
    napi_value options = nullptr;
    if (napi_create_object(env, &options) != napi_ok || options == nullptr) return nullptr;
    napi_set_named_property(env, options, "transfer", value);
    return options;
  }

  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) == napi_ok && type == napi_object) {
    bool has_transfer = false;
    if (napi_has_named_property(env, value, "transfer", &has_transfer) == napi_ok && has_transfer) {
      return value;
    }
  }

  napi_value options = nullptr;
  if (napi_create_object(env, &options) != napi_ok || options == nullptr) return nullptr;
  napi_set_named_property(env, options, "transfer", value);
  return options;
}

napi_value GetTransferListValue(napi_env env, napi_value value) {
  if (value == nullptr || IsUndefinedValue(env, value) || IsNullOrUndefinedValue(env, value)) return nullptr;
  bool is_array = false;
  if (napi_is_array(env, value, &is_array) == napi_ok && is_array) return value;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) == napi_ok && type == napi_object) {
    napi_value transfer = GetNamed(env, value, "transfer");
    if (transfer != nullptr && !IsUndefinedValue(env, transfer) && !IsNullOrUndefinedValue(env, transfer)) {
      return transfer;
    }
  }
  return value;
}

napi_value CloneMessageValue(napi_env env, napi_value value, napi_value transfer_arg) {
  napi_value global = GetGlobal(env);
  napi_value structured_clone = GetNamed(env, global, "structuredClone");
  if (!IsFunction(env, structured_clone)) return value;

  napi_value argv[2] = {value, nullptr};
  size_t argc = 1;
  napi_value options = CreateStructuredCloneOptions(env, transfer_arg);
  if (options != nullptr) {
    argv[1] = options;
    argc = 2;
  }

  napi_value cloned = nullptr;
  if (napi_call_function(env, global, structured_clone, argc, argv, &cloned) != napi_ok || cloned == nullptr) {
    return nullptr;
  }
  return cloned;
}

void EnqueueMessageToPort(napi_env env, MessagePortWrap* wrap, napi_value payload, bool is_close) {
  if (wrap == nullptr) return;
  QueuedMessage queued;
  queued.is_close = is_close;
  if (!is_close && payload != nullptr) {
    if (napi_create_reference(env, payload, 1, &queued.payload_ref) != napi_ok) return;
  }
  {
    std::lock_guard<std::mutex> lock(wrap->mutex);
    if (is_close) {
      if (wrap->close_message_enqueued || wrap->handle_wrap.state != kUbiHandleInitialized) {
        DeleteRefIfPresent(env, &queued.payload_ref);
        return;
      }
      wrap->close_message_enqueued = true;
    }
    wrap->queued_messages.push_back(queued);
  }
  TriggerPortAsync(wrap);
}

void BeginClosePort(napi_env env, MessagePortWrap* wrap, bool notify_peer);
void EmitMessageToPort(napi_env env, napi_value port, napi_value payload, const char* type = "message");

void ProcessQueuedMessages(MessagePortWrap* wrap, bool force) {
  if (wrap == nullptr || wrap->handle_wrap.env == nullptr || wrap->handle_wrap.state != kUbiHandleInitialized) {
    return;
  }

  for (;;) {
    QueuedMessage next;
    bool have_message = false;
    {
      std::lock_guard<std::mutex> lock(wrap->mutex);
      if (wrap->queued_messages.empty()) break;
      if (!force && !wrap->receiving_messages && !wrap->queued_messages.front().is_close) break;
      next = wrap->queued_messages.front();
      wrap->queued_messages.pop_front();
      if (next.is_close) wrap->close_message_enqueued = false;
      have_message = true;
    }
    if (!have_message) break;

    if (next.is_close) {
      DeleteRefIfPresent(wrap->handle_wrap.env, &next.payload_ref);
      BeginClosePort(wrap->handle_wrap.env, wrap, false);
      break;
    }

    napi_value self = UbiHandleWrapGetRefValue(wrap->handle_wrap.env, wrap->handle_wrap.wrapper_ref);
    napi_value payload = GetRefValue(wrap->handle_wrap.env, next.payload_ref);
    DeleteRefIfPresent(wrap->handle_wrap.env, &next.payload_ref);
    if (self == nullptr) continue;
    EmitMessageToPort(wrap->handle_wrap.env,
                      self,
                      payload != nullptr ? payload : Undefined(wrap->handle_wrap.env),
                      "message");
  }
}

void OnMessagePortClosed(uv_handle_t* handle) {
  auto* wrap = static_cast<MessagePortWrap*>(handle != nullptr ? handle->data : nullptr);
  if (wrap == nullptr) return;
  wrap->handle_wrap.state = kUbiHandleClosed;
  UbiHandleWrapReleaseWrapperRef(&wrap->handle_wrap);
  UbiHandleWrapMaybeCallOnClose(&wrap->handle_wrap);
  if (wrap->handle_wrap.active_handle_token != nullptr) {
    UbiUnregisterActiveHandle(wrap->handle_wrap.env, wrap->handle_wrap.active_handle_token);
    wrap->handle_wrap.active_handle_token = nullptr;
  }
  if (wrap->handle_wrap.finalized || wrap->handle_wrap.delete_on_close) {
    DeleteQueuedMessages(wrap->handle_wrap.env, wrap);
    UbiHandleWrapDeleteRefIfPresent(wrap->handle_wrap.env, &wrap->peer_ref);
    UbiHandleWrapDeleteRefIfPresent(wrap->handle_wrap.env, &wrap->handle_wrap.wrapper_ref);
    delete wrap;
  }
}

void OnMessagePortAsync(uv_async_t* handle) {
  auto* wrap = static_cast<MessagePortWrap*>(handle != nullptr ? handle->data : nullptr);
  if (wrap == nullptr) return;
  ProcessQueuedMessages(wrap, false);
}

void DisentanglePeer(napi_env env, MessagePortWrap* wrap, bool enqueue_close) {
  if (wrap == nullptr) return;
  napi_value peer_obj = GetRefValue(env, wrap->peer_ref);
  MessagePortWrap* peer_wrap = UnwrapMessagePort(env, peer_obj);
  DeleteRefIfPresent(env, &wrap->peer_ref);
  if (peer_wrap != nullptr) {
    DeleteRefIfPresent(env, &peer_wrap->peer_ref);
    if (enqueue_close) {
      EnqueueMessageToPort(env, peer_wrap, nullptr, true);
    }
  }
}

void BeginClosePort(napi_env env, MessagePortWrap* wrap, bool notify_peer) {
  if (wrap == nullptr || wrap->handle_wrap.state != kUbiHandleInitialized) return;
  if (notify_peer) {
    DisentanglePeer(env, wrap, true);
  } else {
    DeleteRefIfPresent(env, &wrap->peer_ref);
  }
  wrap->handle_wrap.state = kUbiHandleClosing;
  uv_close(reinterpret_cast<uv_handle_t*>(&wrap->async), OnMessagePortClosed);
}

void MessagePortFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* wrap = static_cast<MessagePortWrap*>(data);
  if (wrap == nullptr) return;
  wrap->handle_wrap.finalized = true;
  UbiHandleWrapDeleteRefIfPresent(env, &wrap->handle_wrap.wrapper_ref);
  if (wrap->handle_wrap.state == kUbiHandleUninitialized || wrap->handle_wrap.state == kUbiHandleClosed) {
    DeleteQueuedMessages(env, wrap);
    DeleteRefIfPresent(env, &wrap->peer_ref);
    if (wrap->handle_wrap.active_handle_token != nullptr) {
      UbiUnregisterActiveHandle(env, wrap->handle_wrap.active_handle_token);
      wrap->handle_wrap.active_handle_token = nullptr;
    }
    delete wrap;
    return;
  }
  wrap->handle_wrap.delete_on_close = true;
  DisentanglePeer(env, wrap, true);
  if (wrap->handle_wrap.state == kUbiHandleInitialized &&
      !uv_is_closing(reinterpret_cast<uv_handle_t*>(&wrap->async))) {
    wrap->handle_wrap.state = kUbiHandleClosing;
    uv_close(reinterpret_cast<uv_handle_t*>(&wrap->async), OnMessagePortClosed);
  }
}

void InvokePortSymbolHook(napi_env env, napi_value port, napi_value symbol) {
  if (port == nullptr || symbol == nullptr) return;
  napi_value hook = nullptr;
  if (napi_get_property(env, port, symbol, &hook) != napi_ok || !IsFunction(env, hook)) return;
  napi_value ignored = nullptr;
  if (napi_call_function(env, port, hook, 0, nullptr, &ignored) != napi_ok) {
    ClearPendingException(env);
  }
}

void EmitMessageToPort(napi_env env, napi_value port, napi_value payload, const char* type) {
  if (port == nullptr) return;

  napi_value emit_message = ResolveEmitMessageValue(env);
  if (IsFunction(env, emit_message)) {
    napi_value ports = nullptr;
    napi_value type_value = nullptr;
    napi_create_array_with_length(env, 0, &ports);
    napi_create_string_utf8(env, type != nullptr ? type : "message", NAPI_AUTO_LENGTH, &type_value);
    napi_value ignored = nullptr;
    napi_value argv[3] = {payload != nullptr ? payload : Undefined(env),
                          ports != nullptr ? ports : Undefined(env),
                          type_value != nullptr ? type_value : Undefined(env)};
    if (napi_call_function(env, port, emit_message, 3, argv, &ignored) == napi_ok) {
      return;
    }
    ClearPendingException(env);
  }

  napi_value event = nullptr;
  if (napi_create_object(env, &event) != napi_ok || event == nullptr) return;
  napi_set_named_property(env, event, "data", payload != nullptr ? payload : Undefined(env));

  napi_value dispatch_event = GetNamed(env, port, "dispatchEvent");
  if (IsFunction(env, dispatch_event)) {
    napi_value ignored = nullptr;
    napi_value argv[1] = {event};
    if (napi_call_function(env, port, dispatch_event, 1, argv, &ignored) == napi_ok) return;
    ClearPendingException(env);
  }

  const char* handler_name = (type != nullptr && strcmp(type, "messageerror") == 0)
                                 ? "onmessageerror"
                                 : "onmessage";
  napi_value handler = GetNamed(env, port, handler_name);
  if (IsFunction(env, handler)) {
    napi_value ignored = nullptr;
    napi_value argv[1] = {event};
    if (napi_call_function(env, port, handler, 1, argv, &ignored) != napi_ok) {
      ClearPendingException(env);
    }
  }
}

void ConnectPorts(napi_env env, napi_value first, napi_value second) {
  MessagePortWrap* first_wrap = UnwrapMessagePort(env, first);
  MessagePortWrap* second_wrap = UnwrapMessagePort(env, second);
  if (first_wrap == nullptr || second_wrap == nullptr) return;
  DeleteRefIfPresent(env, &first_wrap->peer_ref);
  DeleteRefIfPresent(env, &second_wrap->peer_ref);
  napi_create_reference(env, second, 1, &first_wrap->peer_ref);
  napi_create_reference(env, first, 1, &second_wrap->peer_ref);
}

bool EnsureMessagingSymbols(napi_env env, const ResolveOptions& options) {
  auto& state = g_messaging_states[env];
  if (state.no_message_symbol_ref != nullptr &&
      state.oninit_symbol_ref != nullptr &&
      state.handle_onclose_symbol_ref != nullptr) {
    return true;
  }
  napi_value symbols = nullptr;
  if (options.callbacks.resolve_binding != nullptr) {
    symbols = options.callbacks.resolve_binding(env, options.state, "symbols");
  }
  if (symbols == nullptr || IsUndefined(env, symbols)) {
    napi_value global = GetGlobal(env);
    napi_value internal_binding = UbiGetInternalBinding(env);
    if (!IsFunction(env, internal_binding)) {
      internal_binding = GetNamed(env, global, "internalBinding");
    }
    if (IsFunction(env, internal_binding)) {
      napi_value name = nullptr;
      if (napi_create_string_utf8(env, "symbols", NAPI_AUTO_LENGTH, &name) == napi_ok && name != nullptr) {
        napi_value argv[1] = {name};
        napi_call_function(env, global, internal_binding, 1, argv, &symbols);
      }
    }
  }
  if (symbols == nullptr || IsUndefined(env, symbols)) return false;

  SetRefToValue(env, &state.no_message_symbol_ref, GetNamed(env, symbols, "no_message_symbol"));
  SetRefToValue(env, &state.oninit_symbol_ref, GetNamed(env, symbols, "oninit"));
  SetRefToValue(env, &state.handle_onclose_symbol_ref, GetNamed(env, symbols, "handle_onclose"));

  return state.no_message_symbol_ref != nullptr;
}

napi_value MessagePortConstructorCallback(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, nullptr, nullptr, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }

  auto* wrap = new MessagePortWrap();
  UbiHandleWrapInit(&wrap->handle_wrap, env);
  if (napi_wrap(env, this_arg, wrap, MessagePortFinalize, nullptr, &wrap->handle_wrap.wrapper_ref) != napi_ok) {
    delete wrap;
    return nullptr;
  }

  uv_loop_t* loop = UbiGetEnvLoop(env);
  const int rc = loop != nullptr ? uv_async_init(loop, &wrap->async, OnMessagePortAsync) : UV_EINVAL;
  if (rc == 0) {
    wrap->async.data = wrap;
    wrap->handle_wrap.state = kUbiHandleInitialized;
    UbiHandleWrapHoldWrapperRef(&wrap->handle_wrap);
    wrap->handle_wrap.active_handle_token =
        UbiRegisterActiveHandle(env, this_arg, "MESSAGEPORT", MessagePortHasRefActive, MessagePortGetActiveOwner, wrap);
  }

  const napi_value oninit_symbol = GetOnInitSymbol(env);
  if (rc == 0 && oninit_symbol != nullptr) {
    InvokePortSymbolHook(env, this_arg, oninit_symbol);
  }
  return this_arg;
}

napi_value MessagePortPostMessageCallback(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }

  napi_value transfer_list = argc >= 2 ? GetTransferListValue(env, argv[1]) : nullptr;
  if (transfer_list != nullptr && TransferListContainsMarkedUntransferable(env, transfer_list)) {
    napi_value err = CreateDataCloneError(env, "An ArrayBuffer is marked as untransferable");
    if (err != nullptr) {
      napi_throw(env, err);
    }
    return nullptr;
  }

  MessagePortWrap* wrap = UnwrapMessagePort(env, this_arg);
  napi_value payload = (argc >= 1 && argv[0] != nullptr) ? argv[0] : Undefined(env);
  napi_value cloned_payload = CloneMessageValue(env, payload, argc >= 2 ? argv[1] : nullptr);
  if (cloned_payload == nullptr) {
    bool pending = false;
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending) return nullptr;
    cloned_payload = payload;
  }

  if (wrap == nullptr || wrap->handle_wrap.state != kUbiHandleInitialized || wrap->peer_ref == nullptr) {
    return Undefined(env);
  }

  napi_value peer_obj = GetRefValue(env, wrap->peer_ref);
  MessagePortWrap* peer_wrap = UnwrapMessagePort(env, peer_obj);
  if (peer_obj == nullptr || peer_wrap == nullptr || peer_wrap->handle_wrap.state != kUbiHandleInitialized) {
    return Undefined(env);
  }

  EnqueueMessageToPort(env, peer_wrap, cloned_payload, false);
  return Undefined(env);
}

napi_value MessagePortStartCallback(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, nullptr, nullptr, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }
  MessagePortWrap* wrap = UnwrapMessagePort(env, this_arg);
  if (wrap != nullptr && wrap->handle_wrap.state == kUbiHandleInitialized) {
    wrap->receiving_messages = true;
    TriggerPortAsync(wrap);
  }
  return Undefined(env);
}

napi_value MessagePortCloseCallback(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, nullptr, nullptr, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }
  MessagePortWrap* wrap = UnwrapMessagePort(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  BeginClosePort(env, wrap, true);
  return Undefined(env);
}

napi_value MessagePortRefCallback(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, nullptr, nullptr, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }
  MessagePortWrap* wrap = UnwrapMessagePort(env, this_arg);
  if (wrap != nullptr && wrap->handle_wrap.state == kUbiHandleInitialized) {
    uv_ref(reinterpret_cast<uv_handle_t*>(&wrap->async));
  }
  return this_arg;
}

napi_value MessagePortUnrefCallback(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, nullptr, nullptr, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }
  MessagePortWrap* wrap = UnwrapMessagePort(env, this_arg);
  if (wrap != nullptr && wrap->handle_wrap.state == kUbiHandleInitialized) {
    uv_unref(reinterpret_cast<uv_handle_t*>(&wrap->async));
  }
  return this_arg;
}

napi_value MessagePortHasRefCallback(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, nullptr, nullptr, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }
  MessagePortWrap* wrap = UnwrapMessagePort(env, this_arg);
  napi_value out = nullptr;
  napi_get_boolean(env,
                   wrap != nullptr &&
                       UbiHandleWrapHasRef(&wrap->handle_wrap, reinterpret_cast<const uv_handle_t*>(&wrap->async)),
                   &out);
  return out;
}

napi_value MessageChannelConstructorCallback(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, nullptr, nullptr, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }

  auto it = g_messaging_states.find(env);
  if (it == g_messaging_states.end()) return this_arg;
  napi_value message_port_ctor = GetRefValue(env, it->second.message_port_ctor_ref);
  if (!IsFunction(env, message_port_ctor)) return this_arg;

  napi_value port1 = nullptr;
  napi_value port2 = nullptr;
  if (napi_new_instance(env, message_port_ctor, 0, nullptr, &port1) != napi_ok || port1 == nullptr) {
    return this_arg;
  }
  if (napi_new_instance(env, message_port_ctor, 0, nullptr, &port2) != napi_ok || port2 == nullptr) {
    return this_arg;
  }

  ConnectPorts(env, port1, port2);
  napi_set_named_property(env, this_arg, "port1", port1);
  napi_set_named_property(env, this_arg, "port2", port2);
  return this_arg;
}

napi_value BroadcastHandlePostMessageCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }

  napi_value closed_value = nullptr;
  if (napi_get_named_property(env, this_arg, "__ubiClosed", &closed_value) == napi_ok && closed_value != nullptr) {
    bool is_closed = false;
    if (napi_get_value_bool(env, closed_value, &is_closed) == napi_ok && is_closed) {
      return Undefined(env);
    }
  }

  napi_value emit_fn = GetNamed(env, this_arg, "emit");
  if (IsFunction(env, emit_fn)) {
    napi_value event_name = nullptr;
    napi_create_string_utf8(env, "message", NAPI_AUTO_LENGTH, &event_name);
    napi_value payload = (argc >= 1 && argv[0] != nullptr) ? argv[0] : Undefined(env);
    napi_value call_argv[2] = {event_name, payload};
    napi_value ignored = nullptr;
    if (napi_call_function(env, this_arg, emit_fn, 2, call_argv, &ignored) != napi_ok) {
      ClearPendingException(env);
    }
  }

  napi_value true_value = nullptr;
  napi_get_boolean(env, true, &true_value);
  return true_value;
}

napi_value BroadcastHandleCloseCallback(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, nullptr, nullptr, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }
  napi_value true_value = nullptr;
  napi_get_boolean(env, true, &true_value);
  napi_set_named_property(env, this_arg, "__ubiClosed", true_value);
  return Undefined(env);
}

napi_value BroadcastHandleRefCallback(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, nullptr, nullptr, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }
  return this_arg;
}

napi_value BroadcastHandleUnrefCallback(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, nullptr, nullptr, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }
  return this_arg;
}

napi_value CreateEventEmitterInstance(napi_env env) {
  napi_value events_module = TryRequireModule(env, "events");
  if (events_module == nullptr || IsUndefined(env, events_module)) return nullptr;
  napi_value event_emitter_ctor = GetNamed(env, events_module, "EventEmitter");
  if (!IsFunction(env, event_emitter_ctor)) return nullptr;

  napi_value instance = nullptr;
  if (napi_new_instance(env, event_emitter_ctor, 0, nullptr, &instance) != napi_ok || instance == nullptr) {
    return nullptr;
  }
  return instance;
}

napi_value ExposeLazyDOMExceptionPropertyCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) return nullptr;
  if (argc < 1 || argv[0] == nullptr) return Undefined(env);

  napi_valuetype target_type = napi_undefined;
  if (napi_typeof(env, argv[0], &target_type) != napi_ok || target_type != napi_object) return Undefined(env);

  napi_value global = GetGlobal(env);
  napi_value dom_exception = GetNamed(env, global, "DOMException");
  if (dom_exception == nullptr || IsUndefined(env, dom_exception)) {
    dom_exception = ResolveDOMExceptionValue(env);
  }
  if (dom_exception == nullptr || IsUndefined(env, dom_exception)) return Undefined(env);

  napi_property_descriptor desc = {};
  desc.utf8name = "DOMException";
  desc.value = dom_exception;
  desc.attributes = static_cast<napi_property_attributes>(napi_writable | napi_configurable);
  napi_define_properties(env, argv[0], 1, &desc);
  return Undefined(env);
}

napi_value SetDeserializerCreateObjectFunctionCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) return nullptr;

  auto& state = g_messaging_states[env];
  DeleteRefIfPresent(env, &state.deserializer_create_object_ref);
  if (argc >= 1 && IsFunction(env, argv[0])) {
    napi_create_reference(env, argv[0], 1, &state.deserializer_create_object_ref);
  }
  return Undefined(env);
}

bool ApplyArrayBufferTransfers(napi_env env, napi_value options) {
  if (options == nullptr || IsUndefined(env, options)) return true;
  napi_valuetype options_type = napi_undefined;
  if (napi_typeof(env, options, &options_type) != napi_ok || options_type != napi_object) return true;

  napi_value transfer = GetNamed(env, options, "transfer");
  bool is_array = false;
  if (transfer == nullptr || napi_is_array(env, transfer, &is_array) != napi_ok || !is_array) return true;

  uint32_t length = 0;
  if (napi_get_array_length(env, transfer, &length) != napi_ok) return true;

  for (uint32_t i = 0; i < length; ++i) {
    napi_value item = nullptr;
    if (napi_get_element(env, transfer, i, &item) != napi_ok || item == nullptr) continue;
    bool is_arraybuffer = false;
    if (napi_is_arraybuffer(env, item, &is_arraybuffer) != napi_ok || !is_arraybuffer) continue;
    const napi_status detach_status = napi_detach_arraybuffer(env, item);
    if (detach_status == napi_ok) continue;
    bool already_detached = false;
    if (napi_is_detached_arraybuffer(env, item, &already_detached) == napi_ok && already_detached) continue;
    napi_throw_error(env, "ERR_INVALID_STATE", "Failed to transfer detached ArrayBuffer");
    return false;
  }

  return true;
}

napi_value StructuredCloneCallback(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) return nullptr;
  if (argc < 1 || argv[0] == nullptr) return Undefined(env);

  napi_value out = nullptr;
  const napi_status clone_status = unofficial_napi_structured_clone(env, argv[0], &out);
  if (clone_status != napi_ok || out == nullptr) {
    bool has_pending = false;
    if (napi_is_exception_pending(env, &has_pending) == napi_ok && has_pending) return nullptr;
    napi_value err = CreateDataCloneError(env, "The object could not be cloned.");
    if (err != nullptr) napi_throw(env, err);
    return nullptr;
  }

  if (argc >= 2 && !ApplyArrayBufferTransfers(env, argv[1])) {
    return nullptr;
  }
  return out;
}

napi_value BroadcastChannelCallback(napi_env env, napi_callback_info /*info*/) {
  napi_value handle = CreateEventEmitterInstance(env);
  if (handle == nullptr) {
    if (napi_create_object(env, &handle) != napi_ok || handle == nullptr) return Undefined(env);
  }

  napi_value false_value = nullptr;
  napi_get_boolean(env, false, &false_value);
  napi_set_named_property(env, handle, "__ubiClosed", false_value);

  napi_value post_message = nullptr;
  napi_create_function(env,
                       "postMessage",
                       NAPI_AUTO_LENGTH,
                       BroadcastHandlePostMessageCallback,
                       nullptr,
                       &post_message);
  if (post_message != nullptr) napi_set_named_property(env, handle, "postMessage", post_message);

  napi_value close_fn = nullptr;
  napi_create_function(env, "close", NAPI_AUTO_LENGTH, BroadcastHandleCloseCallback, nullptr, &close_fn);
  if (close_fn != nullptr) napi_set_named_property(env, handle, "close", close_fn);

  napi_value ref_fn = nullptr;
  napi_create_function(env, "ref", NAPI_AUTO_LENGTH, BroadcastHandleRefCallback, nullptr, &ref_fn);
  if (ref_fn != nullptr) napi_set_named_property(env, handle, "ref", ref_fn);

  napi_value unref_fn = nullptr;
  napi_create_function(env, "unref", NAPI_AUTO_LENGTH, BroadcastHandleUnrefCallback, nullptr, &unref_fn);
  if (unref_fn != nullptr) napi_set_named_property(env, handle, "unref", unref_fn);

  return handle;
}

napi_value DrainMessagePortCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) return nullptr;
  if (argc < 1 || argv[0] == nullptr) return Undefined(env);
  MessagePortWrap* wrap = UnwrapMessagePort(env, argv[0]);
  if (wrap == nullptr) return Undefined(env);
  ProcessQueuedMessages(wrap, true);
  return Undefined(env);
}

napi_value MoveMessagePortToContextCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) return nullptr;
  if (argc >= 1 && argv[0] != nullptr) return argv[0];
  return Undefined(env);
}

napi_value ReceiveMessageOnPortCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) return nullptr;
  if (argc < 1 || argv[0] == nullptr) {
    napi_value symbol = GetNoMessageSymbol(env);
    return symbol != nullptr ? symbol : Undefined(env);
  }

  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, argv[0], &type) != napi_ok || type != napi_object) {
    napi_throw_type_error(env,
                          "ERR_INVALID_ARG_TYPE",
                          "The \"port\" argument must be a MessagePort instance");
    return nullptr;
  }
  MessagePortWrap* wrap = UnwrapMessagePort(env, argv[0]);
  if (wrap == nullptr) {
    napi_throw_type_error(env,
                          "ERR_INVALID_ARG_TYPE",
                          "The \"port\" argument must be a MessagePort instance");
    return nullptr;
  }

  QueuedMessage next;
  bool have_message = false;
  {
    std::lock_guard<std::mutex> lock(wrap->mutex);
    if (!wrap->queued_messages.empty()) {
      next = wrap->queued_messages.front();
      wrap->queued_messages.pop_front();
      if (next.is_close) wrap->close_message_enqueued = false;
      have_message = true;
    }
  }
  if (!have_message) {
    napi_value symbol = GetNoMessageSymbol(env);
    return symbol != nullptr ? symbol : Undefined(env);
  }

  if (next.is_close) {
    DeleteRefIfPresent(env, &next.payload_ref);
    BeginClosePort(env, wrap, false);
    napi_value symbol = GetNoMessageSymbol(env);
    return symbol != nullptr ? symbol : Undefined(env);
  }

  napi_value value = GetRefValue(env, next.payload_ref);
  DeleteRefIfPresent(env, &next.payload_ref);
  return value != nullptr ? value : Undefined(env);
}

napi_value StopMessagePortCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) return nullptr;
  if (argc >= 1 && argv[0] != nullptr) {
    MessagePortWrap* wrap = UnwrapMessagePort(env, argv[0]);
    if (wrap != nullptr) wrap->receiving_messages = false;
  }
  return Undefined(env);
}

napi_value FallbackDOMExceptionConstructorCallback(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }

  const std::string message =
      (argc >= 1 && argv[0] != nullptr) ? ValueToUtf8(env, argv[0]) : std::string();
  const std::string name =
      (argc >= 2 && argv[1] != nullptr) ? ValueToUtf8(env, argv[1]) : std::string("Error");

  napi_value message_value = nullptr;
  napi_value name_value = nullptr;
  napi_create_string_utf8(env, message.c_str(), NAPI_AUTO_LENGTH, &message_value);
  napi_create_string_utf8(env, name.c_str(), NAPI_AUTO_LENGTH, &name_value);
  if (message_value != nullptr) napi_set_named_property(env, this_arg, "message", message_value);
  if (name_value != nullptr) napi_set_named_property(env, this_arg, "name", name_value);
  SetInt32(env, this_arg, "code", 0);
  return this_arg;
}

napi_value CreateFallbackDOMExceptionConstructor(napi_env env) {
  napi_value ctor = nullptr;
  if (napi_define_class(env,
                        "DOMException",
                        NAPI_AUTO_LENGTH,
                        FallbackDOMExceptionConstructorCallback,
                        nullptr,
                        0,
                        nullptr,
                        &ctor) != napi_ok ||
      ctor == nullptr) {
    return nullptr;
  }

  napi_value global = GetGlobal(env);
  napi_value object_ctor = GetNamed(env, global, "Object");
  napi_value set_prototype_of = GetNamed(env, object_ctor, "setPrototypeOf");
  napi_value error_ctor = GetNamed(env, global, "Error");
  napi_value error_prototype = GetNamed(env, error_ctor, "prototype");
  napi_value dom_prototype = GetNamed(env, ctor, "prototype");
  if (IsFunction(env, set_prototype_of) && dom_prototype != nullptr && error_prototype != nullptr) {
    napi_value argv[2] = {dom_prototype, error_prototype};
    napi_value ignored = nullptr;
    if (napi_call_function(env, object_ctor, set_prototype_of, 2, argv, &ignored) != napi_ok) {
      ClearPendingException(env);
    }
  }
  return ctor;
}

napi_value ResolveDOMExceptionValue(napi_env env) {
  napi_value global = GetGlobal(env);
  napi_value dom_exception = GetNamed(env, global, "DOMException");
  if (dom_exception != nullptr && !IsUndefined(env, dom_exception)) return dom_exception;

  napi_value dom_module = TryRequireModule(env, "internal/per_context/domexception");
  if (dom_module != nullptr && !IsUndefined(env, dom_module)) {
    dom_exception = GetNamed(env, dom_module, "DOMException");
    if (dom_exception != nullptr && !IsUndefined(env, dom_exception)) return dom_exception;
    dom_exception = GetNamed(env, dom_module, "default");
    if (dom_exception != nullptr && !IsUndefined(env, dom_exception)) return dom_exception;
  }

  dom_exception = CreateFallbackDOMExceptionConstructor(env);
  if (dom_exception != nullptr && !IsUndefined(env, dom_exception)) {
    napi_set_named_property(env, global, "DOMException", dom_exception);
    return dom_exception;
  }

  return Undefined(env);
}

napi_value ResolveEmitMessageValue(napi_env env) {
  auto& state = g_messaging_states[env];
  napi_value cached = GetRefValue(env, state.emit_message_ref);
  if (IsFunction(env, cached)) return cached;

  napi_value messageport_module = TryRequireModule(env, "internal/per_context/messageport");
  if (messageport_module == nullptr || IsUndefined(env, messageport_module)) return nullptr;

  napi_value emit_message = GetNamed(env, messageport_module, "emitMessage");
  if (!IsFunction(env, emit_message)) return nullptr;

  SetRefToValue(env, &state.emit_message_ref, emit_message);
  return emit_message;
}

napi_value GetCachedMessaging(napi_env env) {
  auto it = g_messaging_states.find(env);
  if (it == g_messaging_states.end() || it->second.binding_ref == nullptr) return nullptr;
  return GetRefValue(env, it->second.binding_ref);
}

}  // namespace

napi_value ResolveMessaging(napi_env env, const ResolveOptions& options) {
  const napi_value undefined = Undefined(env);
  napi_value cached = GetCachedMessaging(env);
  if (cached != nullptr) return cached;

  auto& state = g_messaging_states[env];
  EnsureMessagingSymbols(env, options);

  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) return undefined;

  napi_value message_port_ctor = nullptr;
  if (napi_define_class(env,
                        "MessagePort",
                        NAPI_AUTO_LENGTH,
                        MessagePortConstructorCallback,
                        nullptr,
                        0,
                        nullptr,
                        &message_port_ctor) == napi_ok &&
      message_port_ctor != nullptr) {
    constexpr napi_property_attributes kMutableMethodAttrs =
        static_cast<napi_property_attributes>(napi_writable | napi_configurable);
    napi_property_descriptor methods[] = {
        {"postMessage", nullptr, MessagePortPostMessageCallback, nullptr, nullptr, nullptr, kMutableMethodAttrs, nullptr},
        {"start", nullptr, MessagePortStartCallback, nullptr, nullptr, nullptr, kMutableMethodAttrs, nullptr},
        {"close", nullptr, MessagePortCloseCallback, nullptr, nullptr, nullptr, kMutableMethodAttrs, nullptr},
        {"ref", nullptr, MessagePortRefCallback, nullptr, nullptr, nullptr, kMutableMethodAttrs, nullptr},
        {"unref", nullptr, MessagePortUnrefCallback, nullptr, nullptr, nullptr, kMutableMethodAttrs, nullptr},
        {"hasRef", nullptr, MessagePortHasRefCallback, nullptr, nullptr, nullptr, kMutableMethodAttrs, nullptr},
    };
    napi_value prototype = nullptr;
    if (napi_get_named_property(env, message_port_ctor, "prototype", &prototype) == napi_ok && prototype != nullptr) {
      napi_define_properties(env, prototype, sizeof(methods) / sizeof(methods[0]), methods);
    }
    napi_set_named_property(env, out, "MessagePort", message_port_ctor);
    SetRefToValue(env, &state.message_port_ctor_ref, message_port_ctor);
  }

  napi_value message_channel_ctor = nullptr;
  if (napi_define_class(env,
                        "MessageChannel",
                        NAPI_AUTO_LENGTH,
                        MessageChannelConstructorCallback,
                        nullptr,
                        0,
                        nullptr,
                        &message_channel_ctor) == napi_ok &&
      message_channel_ctor != nullptr) {
    napi_set_named_property(env, out, "MessageChannel", message_channel_ctor);
  }

  napi_value broadcast_channel_fn = nullptr;
  if (napi_create_function(env,
                           "broadcastChannel",
                           NAPI_AUTO_LENGTH,
                           BroadcastChannelCallback,
                           nullptr,
                           &broadcast_channel_fn) == napi_ok &&
      broadcast_channel_fn != nullptr) {
    napi_set_named_property(env, out, "broadcastChannel", broadcast_channel_fn);
  }

  napi_value drain_fn = nullptr;
  if (napi_create_function(env,
                           "drainMessagePort",
                           NAPI_AUTO_LENGTH,
                           DrainMessagePortCallback,
                           nullptr,
                           &drain_fn) == napi_ok &&
      drain_fn != nullptr) {
    napi_set_named_property(env, out, "drainMessagePort", drain_fn);
  }

  napi_value move_fn = nullptr;
  if (napi_create_function(env,
                           "moveMessagePortToContext",
                           NAPI_AUTO_LENGTH,
                           MoveMessagePortToContextCallback,
                           nullptr,
                           &move_fn) == napi_ok &&
      move_fn != nullptr) {
    napi_set_named_property(env, out, "moveMessagePortToContext", move_fn);
  }

  napi_value receive_fn = nullptr;
  if (napi_create_function(env,
                           "receiveMessageOnPort",
                           NAPI_AUTO_LENGTH,
                           ReceiveMessageOnPortCallback,
                           nullptr,
                           &receive_fn) == napi_ok &&
      receive_fn != nullptr) {
    napi_set_named_property(env, out, "receiveMessageOnPort", receive_fn);
  }

  napi_value stop_fn = nullptr;
  if (napi_create_function(env,
                           "stopMessagePort",
                           NAPI_AUTO_LENGTH,
                           StopMessagePortCallback,
                           nullptr,
                           &stop_fn) == napi_ok &&
      stop_fn != nullptr) {
    napi_set_named_property(env, out, "stopMessagePort", stop_fn);
  }

  napi_value expose_fn = nullptr;
  if (napi_create_function(env,
                           "exposeLazyDOMExceptionProperty",
                           NAPI_AUTO_LENGTH,
                           ExposeLazyDOMExceptionPropertyCallback,
                           nullptr,
                           &expose_fn) == napi_ok &&
      expose_fn != nullptr) {
    napi_set_named_property(env, out, "exposeLazyDOMExceptionProperty", expose_fn);
  }

  napi_value set_deserializer = nullptr;
  if (napi_create_function(env,
                           "setDeserializerCreateObjectFunction",
                           NAPI_AUTO_LENGTH,
                           SetDeserializerCreateObjectFunctionCallback,
                           nullptr,
                           &set_deserializer) == napi_ok &&
      set_deserializer != nullptr) {
    napi_set_named_property(env, out, "setDeserializerCreateObjectFunction", set_deserializer);
  }

  napi_value structured_clone = nullptr;
  if (napi_create_function(env,
                           "structuredClone",
                           NAPI_AUTO_LENGTH,
                           StructuredCloneCallback,
                           nullptr,
                           &structured_clone) == napi_ok &&
      structured_clone != nullptr) {
    napi_set_named_property(env, out, "structuredClone", structured_clone);
  }

  napi_value dom_exception = ResolveDOMExceptionValue(env);
  if (dom_exception != nullptr && !IsUndefined(env, dom_exception)) {
    napi_set_named_property(env, out, "DOMException", dom_exception);
  }

  SetRefToValue(env, &state.binding_ref, out);
  return out;
}

}  // namespace internal_binding
