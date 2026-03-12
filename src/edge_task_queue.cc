#include "edge_task_queue.h"

#include "edge_environment.h"
#include "internal_binding/helpers.h"
#include "unofficial_napi.h"

namespace {

void DeleteRefIfAny(napi_env env, napi_ref* ref_slot);

struct TaskQueueBindingState {
  explicit TaskQueueBindingState(napi_env env_in) : env(env_in) {}
  ~TaskQueueBindingState() {
    DeleteRefIfAny(env, &binding_ref);
    DeleteRefIfAny(env, &tick_callback_ref);
    DeleteRefIfAny(env, &promise_reject_callback_ref);
    tick_info_fields = nullptr;
    if (auto* environment = EdgeEnvironmentGet(env); environment != nullptr) {
      environment->tick_info()->fields = nullptr;
      DeleteRefIfAny(env, &environment->tick_info()->ref);
    }
  }

  napi_env env = nullptr;
  napi_ref binding_ref = nullptr;
  napi_ref tick_callback_ref = nullptr;
  napi_ref promise_reject_callback_ref = nullptr;
  int32_t* tick_info_fields = nullptr;
};

void DeleteRefIfAny(napi_env env, napi_ref* ref_slot) {
  if (env == nullptr || ref_slot == nullptr || *ref_slot == nullptr) return;
  napi_delete_reference(env, *ref_slot);
  *ref_slot = nullptr;
}

TaskQueueBindingState& GetTaskQueueState(napi_env env) {
  return EdgeEnvironmentGetOrCreateSlotData<TaskQueueBindingState>(
      env, kEdgeEnvironmentSlotTaskQueueBindingState);
}

bool CreateInt32Array(napi_env env, uint32_t length, napi_value* out_array, int32_t** out_data) {
  if (out_array == nullptr || out_data == nullptr) return false;
  *out_array = nullptr;
  *out_data = nullptr;

  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) return false;

  napi_value int32_array_ctor = nullptr;
  if (napi_get_named_property(env, global, "Int32Array", &int32_array_ctor) != napi_ok ||
      int32_array_ctor == nullptr) {
    return false;
  }

  napi_value js_length = nullptr;
  if (napi_create_uint32(env, length, &js_length) != napi_ok || js_length == nullptr) return false;

  napi_value argv[1] = {js_length};
  napi_value array = nullptr;
  if (napi_new_instance(env, int32_array_ctor, 1, argv, &array) != napi_ok || array == nullptr) return false;

  napi_typedarray_type type = napi_uint8_array;
  size_t actual_length = 0;
  void* data = nullptr;
  napi_value arraybuffer = nullptr;
  size_t byte_offset = 0;
  if (napi_get_typedarray_info(env, array, &type, &actual_length, &data, &arraybuffer, &byte_offset) != napi_ok ||
      type != napi_int32_array || actual_length < length || data == nullptr) {
    return false;
  }

  auto* fields = static_cast<int32_t*>(data);
  for (uint32_t i = 0; i < length; ++i) {
    fields[i] = 0;
  }

  *out_array = array;
  *out_data = fields;
  return true;
}

static napi_value TaskQueueEnqueueMicrotask(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) return nullptr;

#if defined(EDGE_BUNDLED_NAPI_V8)
  if (unofficial_napi_enqueue_microtask(env, argv[0]) == napi_ok) {
    return internal_binding::Undefined(env);
  }
#endif

  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) return nullptr;

  napi_value queue_microtask = nullptr;
  if (napi_get_named_property(env, global, "queueMicrotask", &queue_microtask) == napi_ok &&
      queue_microtask != nullptr) {
    napi_valuetype t = napi_undefined;
    if (napi_typeof(env, queue_microtask, &t) == napi_ok && t == napi_function) {
      napi_value ignored = nullptr;
      napi_call_function(env, global, queue_microtask, 1, argv, &ignored);
    }
  }

  return internal_binding::Undefined(env);
}

static napi_value TaskQueueRunMicrotasks(napi_env env, napi_callback_info /*info*/) {
  (void)unofficial_napi_process_microtasks(env);
  return internal_binding::Undefined(env);
}

static napi_value TaskQueueSetTickCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) return nullptr;

  auto& st = GetTaskQueueState(env);
  DeleteRefIfAny(env, &st.tick_callback_ref);

  napi_valuetype t = napi_undefined;
  if (napi_typeof(env, argv[0], &t) == napi_ok && t == napi_function) {
    napi_create_reference(env, argv[0], 1, &st.tick_callback_ref);
  }

  return internal_binding::Undefined(env);
}

static napi_value TaskQueueSetPromiseRejectCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) return nullptr;

#if defined(EDGE_BUNDLED_NAPI_V8)
  (void)unofficial_napi_set_promise_reject_callback(env, argv[0]);
#endif

  auto& st = GetTaskQueueState(env);
  DeleteRefIfAny(env, &st.promise_reject_callback_ref);

  napi_valuetype t = napi_undefined;
  if (napi_typeof(env, argv[0], &t) == napi_ok && t == napi_function) {
    napi_create_reference(env, argv[0], 1, &st.promise_reject_callback_ref);
  }

  return internal_binding::Undefined(env);
}

}  // namespace

napi_value EdgeGetOrCreateTaskQueueBinding(napi_env env) {
  if (env == nullptr) return nullptr;

  auto& st = GetTaskQueueState(env);
  if (st.binding_ref != nullptr) {
    napi_value existing = nullptr;
    if (napi_get_reference_value(env, st.binding_ref, &existing) == napi_ok && existing != nullptr) {
      return existing;
    }
  }

  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) return nullptr;

  auto define_method = [&](const char* name, napi_callback cb) -> bool {
    napi_value fn = nullptr;
    if (napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, nullptr, &fn) != napi_ok || fn == nullptr) {
      return false;
    }
    return napi_set_named_property(env, binding, name, fn) == napi_ok;
  };

  if (!define_method("enqueueMicrotask", TaskQueueEnqueueMicrotask) ||
      !define_method("setTickCallback", TaskQueueSetTickCallback) ||
      !define_method("runMicrotasks", TaskQueueRunMicrotasks) ||
      !define_method("setPromiseRejectCallback", TaskQueueSetPromiseRejectCallback)) {
    return nullptr;
  }

  napi_value tick_info = nullptr;
  int32_t* tick_info_fields = nullptr;
  if (!CreateInt32Array(env, 2, &tick_info, &tick_info_fields)) return nullptr;
  st.tick_info_fields = tick_info_fields;
  if (auto* environment = EdgeEnvironmentGet(env); environment != nullptr) {
    environment->tick_info()->fields = tick_info_fields;
  }
  if (napi_set_named_property(env, binding, "tickInfo", tick_info) != napi_ok) return nullptr;
  if (auto* environment = EdgeEnvironmentGet(env); environment != nullptr) {
    DeleteRefIfAny(env, &environment->tick_info()->ref);
    napi_create_reference(env, tick_info, 1, &environment->tick_info()->ref);
  }

  napi_value promise_events = nullptr;
  if (napi_create_object(env, &promise_events) != napi_ok || promise_events == nullptr) return nullptr;
  auto set_event_const = [&](const char* name, int32_t value) -> bool {
    napi_value v = nullptr;
    return napi_create_int32(env, value, &v) == napi_ok && v != nullptr &&
           napi_set_named_property(env, promise_events, name, v) == napi_ok;
  };
  if (!set_event_const("kPromiseRejectWithNoHandler", 0) ||
      !set_event_const("kPromiseHandlerAddedAfterReject", 1) ||
      !set_event_const("kPromiseResolveAfterResolved", 2) ||
      !set_event_const("kPromiseRejectAfterResolved", 3)) {
    return nullptr;
  }
  if (napi_set_named_property(env, binding, "promiseRejectEvents", promise_events) != napi_ok) return nullptr;

  DeleteRefIfAny(env, &st.binding_ref);
  if (napi_create_reference(env, binding, 1, &st.binding_ref) != napi_ok || st.binding_ref == nullptr) {
    return nullptr;
  }

  return binding;
}

napi_status EdgeRunTaskQueueTickCallback(napi_env env, bool* called) {
  if (called != nullptr) {
    *called = false;
  }
  if (env == nullptr) {
    return napi_invalid_arg;
  }

  auto* state = EdgeEnvironmentGetSlotData<TaskQueueBindingState>(
      env, kEdgeEnvironmentSlotTaskQueueBindingState);
  if (state == nullptr || state->tick_callback_ref == nullptr) {
    return napi_ok;
  }

  napi_value tick_cb = nullptr;
  napi_status status = napi_get_reference_value(env, state->tick_callback_ref, &tick_cb);
  if (status != napi_ok || tick_cb == nullptr) {
    return status == napi_ok ? napi_generic_failure : status;
  }

  napi_value global = nullptr;
  status = napi_get_global(env, &global);
  if (status != napi_ok || global == nullptr) {
    return status == napi_ok ? napi_generic_failure : status;
  }

  napi_value process = nullptr;
  if (napi_get_named_property(env, global, "process", &process) != napi_ok || process == nullptr) {
    process = global;
  }

  napi_value ignored = nullptr;
  status = napi_call_function(env, process, tick_cb, 0, nullptr, &ignored);
  if (status == napi_ok && called != nullptr) {
    *called = true;
  }
  return status;
}

bool EdgeGetTaskQueueFlags(napi_env env, bool* has_tick_scheduled, bool* has_rejection_to_warn) {
  if (has_tick_scheduled != nullptr) {
    *has_tick_scheduled = false;
  }
  if (has_rejection_to_warn != nullptr) {
    *has_rejection_to_warn = false;
  }
  if (env == nullptr) {
    return false;
  }

  int32_t* tick_info_fields = nullptr;
  if (auto* environment = EdgeEnvironmentGet(env); environment != nullptr) {
    tick_info_fields = environment->tick_info()->fields;
  }
  auto* state = EdgeEnvironmentGetSlotData<TaskQueueBindingState>(
      env, kEdgeEnvironmentSlotTaskQueueBindingState);
  if (tick_info_fields == nullptr && (state == nullptr || state->tick_info_fields == nullptr)) {
    return false;
  }
  if (tick_info_fields == nullptr) tick_info_fields = state->tick_info_fields;

  if (has_tick_scheduled != nullptr) {
    *has_tick_scheduled = tick_info_fields[0] != 0;
  }
  if (has_rejection_to_warn != nullptr) {
    *has_rejection_to_warn = tick_info_fields[1] != 0;
  }
  return true;
}
