#include "internal_binding/dispatch.h"

#include <unordered_map>

#include "internal_binding/helpers.h"

namespace internal_binding {

namespace {

struct WorkerState {
  napi_ref binding_ref = nullptr;
  napi_ref env_message_port_ref = nullptr;
};

std::unordered_map<napi_env, WorkerState> g_worker_states;

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

void SetRefValue(napi_env env, napi_ref* ref, napi_value value) {
  if (ref == nullptr) return;
  if (*ref != nullptr) {
    napi_delete_reference(env, *ref);
    *ref = nullptr;
  }
  if (value != nullptr) {
    napi_create_reference(env, value, 1, ref);
  }
}

napi_value GetCachedWorker(napi_env env) {
  auto it = g_worker_states.find(env);
  if (it == g_worker_states.end() || it->second.binding_ref == nullptr) return nullptr;
  napi_value out = nullptr;
  if (napi_get_reference_value(env, it->second.binding_ref, &out) != napi_ok || out == nullptr) return nullptr;
  return out;
}

napi_value GetOrCreateEnvMessagePort(napi_env env) {
  auto& state = g_worker_states[env];
  if (state.env_message_port_ref != nullptr) {
    napi_value out = nullptr;
    if (napi_get_reference_value(env, state.env_message_port_ref, &out) == napi_ok && out != nullptr) {
      return out;
    }
  }

  napi_value global = GetGlobal(env);
  napi_value internal_binding = GetNamed(env, global, "internalBinding");
  if (!IsFunction(env, internal_binding)) return Undefined(env);

  napi_value messaging_name = nullptr;
  if (napi_create_string_utf8(env, "messaging", NAPI_AUTO_LENGTH, &messaging_name) != napi_ok ||
      messaging_name == nullptr) {
    return Undefined(env);
  }

  napi_value messaging = nullptr;
  napi_value argv[1] = {messaging_name};
  if (napi_call_function(env, global, internal_binding, 1, argv, &messaging) != napi_ok || messaging == nullptr) {
    return Undefined(env);
  }

  napi_value channel_ctor = GetNamed(env, messaging, "MessageChannel");
  if (!IsFunction(env, channel_ctor)) return Undefined(env);

  napi_value channel = nullptr;
  if (napi_new_instance(env, channel_ctor, 0, nullptr, &channel) != napi_ok || channel == nullptr) {
    return Undefined(env);
  }

  napi_value port1 = GetNamed(env, channel, "port1");
  if (port1 == nullptr) return Undefined(env);
  SetRefValue(env, &state.env_message_port_ref, port1);
  return port1;
}

napi_value WorkerGetEnvMessagePort(napi_env env, napi_callback_info /*info*/) {
  return GetOrCreateEnvMessagePort(env);
}

}  // namespace

napi_value ResolveWorker(napi_env env, const ResolveOptions& /*options*/) {
  const napi_value undefined = Undefined(env);
  napi_value cached = GetCachedWorker(env);
  if (cached != nullptr) return cached;

  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) return undefined;

  SetBool(env, out, "isMainThread", true);
  SetBool(env, out, "isInternalThread", false);
  SetBool(env, out, "ownsProcessState", true);
  SetInt32(env, out, "threadId", 0);
  SetString(env, out, "threadName", "main");

  napi_value resource_limits = nullptr;
  if (napi_create_object(env, &resource_limits) == napi_ok && resource_limits != nullptr) {
    napi_set_named_property(env, out, "resourceLimits", resource_limits);
  }

  SetInt32(env, out, "kMaxYoungGenerationSizeMb", 0);
  SetInt32(env, out, "kMaxOldGenerationSizeMb", 1);
  SetInt32(env, out, "kCodeRangeSizeMb", 2);
  SetInt32(env, out, "kStackSizeMb", 3);
  SetInt32(env, out, "kTotalResourceLimitCount", 4);

  napi_value get_env_message_port = nullptr;
  if (napi_create_function(env,
                           "getEnvMessagePort",
                           NAPI_AUTO_LENGTH,
                           WorkerGetEnvMessagePort,
                           nullptr,
                           &get_env_message_port) == napi_ok &&
      get_env_message_port != nullptr) {
    napi_set_named_property(env, out, "getEnvMessagePort", get_env_message_port);
  }

  auto& state = g_worker_states[env];
  SetRefValue(env, &state.binding_ref, out);
  return out;
}

}  // namespace internal_binding
