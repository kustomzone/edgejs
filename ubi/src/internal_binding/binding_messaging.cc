#include "internal_binding/dispatch.h"

#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "internal_binding/helpers.h"
#include "../ubi_module_loader.h"

namespace internal_binding {

namespace {

struct MessagePortWrap {
  napi_ref peer_ref = nullptr;
  std::deque<napi_ref> queued_messages;
  bool closed = false;
  bool refed = true;
};

struct MessagingState {
  napi_ref binding_ref = nullptr;
  napi_ref deserializer_create_object_ref = nullptr;
  napi_ref message_port_ctor_ref = nullptr;
  napi_ref no_message_symbol_ref = nullptr;
  napi_ref oninit_symbol_ref = nullptr;
  napi_ref handle_onclose_symbol_ref = nullptr;
};

std::unordered_map<napi_env, MessagingState> g_messaging_states;

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
  napi_value require_fn = GetNamed(env, global, "require");
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

void MessagePortFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* wrap = static_cast<MessagePortWrap*>(data);
  if (wrap == nullptr) return;
  DeleteRefIfPresent(env, &wrap->peer_ref);
  for (napi_ref ref : wrap->queued_messages) {
    if (ref != nullptr) napi_delete_reference(env, ref);
  }
  delete wrap;
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

void EmitMessageToPort(napi_env env, napi_value port, napi_value payload) {
  if (port == nullptr) return;

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

  napi_value onmessage = GetNamed(env, port, "onmessage");
  if (IsFunction(env, onmessage)) {
    napi_value ignored = nullptr;
    napi_value argv[1] = {event};
    if (napi_call_function(env, port, onmessage, 1, argv, &ignored) != napi_ok) {
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
  if (napi_wrap(env, this_arg, wrap, MessagePortFinalize, nullptr, nullptr) != napi_ok) {
    delete wrap;
    return nullptr;
  }

  const napi_value oninit_symbol = GetOnInitSymbol(env);
  if (oninit_symbol != nullptr) {
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

  if (argc >= 2 && TransferListContainsMarkedUntransferable(env, argv[1])) {
    napi_value err = CreateDataCloneError(env, "An ArrayBuffer is marked as untransferable");
    if (err != nullptr) {
      napi_throw(env, err);
    }
    return nullptr;
  }

  MessagePortWrap* wrap = UnwrapMessagePort(env, this_arg);
  if (wrap == nullptr || wrap->closed || wrap->peer_ref == nullptr) return Undefined(env);

  napi_value peer_obj = GetRefValue(env, wrap->peer_ref);
  MessagePortWrap* peer_wrap = UnwrapMessagePort(env, peer_obj);
  if (peer_obj == nullptr || peer_wrap == nullptr || peer_wrap->closed) return Undefined(env);

  napi_value payload = (argc >= 1 && argv[0] != nullptr) ? argv[0] : Undefined(env);
  napi_ref payload_ref = nullptr;
  if (napi_create_reference(env, payload, 1, &payload_ref) == napi_ok && payload_ref != nullptr) {
    peer_wrap->queued_messages.push_back(payload_ref);
  }
  EmitMessageToPort(env, peer_obj, payload);
  return Undefined(env);
}

napi_value MessagePortStartCallback(napi_env env, napi_callback_info /*info*/) {
  return Undefined(env);
}

napi_value MessagePortCloseCallback(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, nullptr, nullptr, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }
  MessagePortWrap* wrap = UnwrapMessagePort(env, this_arg);
  if (wrap == nullptr || wrap->closed) return Undefined(env);
  wrap->closed = true;

  if (wrap->peer_ref != nullptr) {
    napi_value peer_obj = GetRefValue(env, wrap->peer_ref);
    MessagePortWrap* peer_wrap = UnwrapMessagePort(env, peer_obj);
    if (peer_wrap != nullptr) {
      DeleteRefIfPresent(env, &peer_wrap->peer_ref);
    }
  }
  DeleteRefIfPresent(env, &wrap->peer_ref);

  const napi_value onclose_symbol = GetHandleOnCloseSymbol(env);
  if (onclose_symbol != nullptr) {
    InvokePortSymbolHook(env, this_arg, onclose_symbol);
  }
  return Undefined(env);
}

napi_value MessagePortRefCallback(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, nullptr, nullptr, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }
  MessagePortWrap* wrap = UnwrapMessagePort(env, this_arg);
  if (wrap != nullptr) wrap->refed = true;
  return this_arg;
}

napi_value MessagePortUnrefCallback(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, nullptr, nullptr, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }
  MessagePortWrap* wrap = UnwrapMessagePort(env, this_arg);
  if (wrap != nullptr) wrap->refed = false;
  return this_arg;
}

napi_value MessagePortHasRefCallback(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, nullptr, nullptr, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }
  MessagePortWrap* wrap = UnwrapMessagePort(env, this_arg);
  napi_value out = nullptr;
  napi_get_boolean(env, wrap != nullptr && wrap->refed, &out);
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
  if (dom_exception == nullptr || IsUndefined(env, dom_exception)) return Undefined(env);
  napi_set_named_property(env, argv[0], "DOMException", dom_exception);
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

  if (argc >= 2 && !ApplyArrayBufferTransfers(env, argv[1])) {
    return nullptr;
  }

  napi_value global = GetGlobal(env);
  napi_value json = GetNamed(env, global, "JSON");
  napi_value stringify = GetNamed(env, json, "stringify");
  napi_value parse = GetNamed(env, json, "parse");
  if (IsFunction(env, stringify) && IsFunction(env, parse)) {
    napi_value json_text = nullptr;
    if (napi_call_function(env, json, stringify, 1, argv, &json_text) == napi_ok &&
        json_text != nullptr) {
      napi_value parse_argv[1] = {json_text};
      napi_value out = nullptr;
      if (napi_call_function(env, json, parse, 1, parse_argv, &out) == napi_ok && out != nullptr) {
        return out;
      }
      bool has_pending = false;
      if (napi_is_exception_pending(env, &has_pending) == napi_ok && has_pending) return nullptr;
    } else {
      bool has_pending = false;
      if (napi_is_exception_pending(env, &has_pending) == napi_ok && has_pending) return nullptr;
    }
  }

  return argv[0];
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
  for (napi_ref ref : wrap->queued_messages) {
    if (ref != nullptr) napi_delete_reference(env, ref);
  }
  wrap->queued_messages.clear();
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

  MessagePortWrap* wrap = UnwrapMessagePort(env, argv[0]);
  if (wrap == nullptr || wrap->queued_messages.empty()) {
    napi_value symbol = GetNoMessageSymbol(env);
    return symbol != nullptr ? symbol : Undefined(env);
  }

  napi_ref message_ref = wrap->queued_messages.front();
  wrap->queued_messages.pop_front();
  napi_value value = nullptr;
  if (message_ref != nullptr) {
    napi_get_reference_value(env, message_ref, &value);
    napi_delete_reference(env, message_ref);
  }
  return value != nullptr ? value : Undefined(env);
}

napi_value StopMessagePortCallback(napi_env env, napi_callback_info /*info*/) {
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
