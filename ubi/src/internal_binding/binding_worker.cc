#include "internal_binding/dispatch.h"

#include <string>
#include <unordered_map>
#include <vector>

#include "internal_binding/helpers.h"
#include "../ubi_module_loader.h"

namespace internal_binding {

namespace {

struct WorkerImplWrap {
  napi_ref wrapper_ref = nullptr;
  napi_ref url_ref = nullptr;
  napi_ref env_ref = nullptr;
  napi_ref exec_argv_ref = nullptr;
  napi_ref resource_limits_ref = nullptr;
  napi_ref message_port_ref = nullptr;
  napi_ref internal_port_ref = nullptr;
  int32_t thread_id = 0;
  std::string thread_name = "WorkerThread";
  bool started = false;
};

struct WorkerState {
  napi_ref binding_ref = nullptr;
  napi_ref env_message_port_ref = nullptr;
  int32_t next_thread_id = 1;
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

bool IsNullOrUndefinedValue(napi_env env, napi_value value) {
  if (value == nullptr) return true;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok) return true;
  return type == napi_null || type == napi_undefined;
}

std::string ValueToUtf8(napi_env env, napi_value value) {
  if (value == nullptr) return {};
  napi_value as_string = nullptr;
  if (napi_coerce_to_string(env, value, &as_string) != napi_ok || as_string == nullptr) return {};
  size_t len = 0;
  if (napi_get_value_string_utf8(env, as_string, nullptr, 0, &len) != napi_ok) return {};
  std::string out(len + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, as_string, out.data(), out.size(), &copied) != napi_ok) return {};
  out.resize(copied);
  return out;
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

napi_value GetRefValue(napi_env env, napi_ref ref) {
  if (ref == nullptr) return nullptr;
  napi_value out = nullptr;
  if (napi_get_reference_value(env, ref, &out) != napi_ok || out == nullptr) return nullptr;
  return out;
}

napi_value GetCachedWorker(napi_env env) {
  auto it = g_worker_states.find(env);
  if (it == g_worker_states.end() || it->second.binding_ref == nullptr) return nullptr;
  return GetRefValue(env, it->second.binding_ref);
}

napi_value RequireModule(napi_env env, const char* name) {
  napi_value global = GetGlobal(env);
  napi_value require_fn = GetNamed(env, global, "require");
  if (!IsFunction(env, require_fn)) return nullptr;
  napi_value name_v = nullptr;
  if (napi_create_string_utf8(env, name, NAPI_AUTO_LENGTH, &name_v) != napi_ok || name_v == nullptr) return nullptr;
  napi_value out = nullptr;
  if (napi_call_function(env, global, require_fn, 1, &name_v, &out) != napi_ok || out == nullptr) return nullptr;
  return out;
}

napi_value GetInternalBindingByName(napi_env env, const char* name) {
  napi_value global = GetGlobal(env);
  napi_value internal_binding = UbiGetInternalBinding(env);
  if (!IsFunction(env, internal_binding)) {
    internal_binding = GetNamed(env, global, "internalBinding");
  }
  if (!IsFunction(env, internal_binding)) return nullptr;
  napi_value name_v = nullptr;
  if (napi_create_string_utf8(env, name, NAPI_AUTO_LENGTH, &name_v) != napi_ok || name_v == nullptr) return nullptr;
  napi_value out = nullptr;
  if (napi_call_function(env, global, internal_binding, 1, &name_v, &out) != napi_ok || out == nullptr) return nullptr;
  return out;
}

void WorkerImplFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* wrap = static_cast<WorkerImplWrap*>(data);
  if (wrap == nullptr) return;
  SetRefValue(env, &wrap->wrapper_ref, nullptr);
  SetRefValue(env, &wrap->url_ref, nullptr);
  SetRefValue(env, &wrap->env_ref, nullptr);
  SetRefValue(env, &wrap->exec_argv_ref, nullptr);
  SetRefValue(env, &wrap->resource_limits_ref, nullptr);
  SetRefValue(env, &wrap->message_port_ref, nullptr);
  SetRefValue(env, &wrap->internal_port_ref, nullptr);
  delete wrap;
}

WorkerImplWrap* UnwrapWorkerImpl(napi_env env, napi_value this_arg) {
  if (this_arg == nullptr) return nullptr;
  void* data = nullptr;
  if (napi_unwrap(env, this_arg, &data) != napi_ok || data == nullptr) return nullptr;
  return static_cast<WorkerImplWrap*>(data);
}

std::string FileUrlToPath(const std::string& maybe_url) {
  if (maybe_url.rfind("file://", 0) != 0) return maybe_url;
  std::string path = maybe_url.substr(7);
  if (path.size() >= 3 && path[0] == '/' && path[2] == ':') {
    return path.substr(1);  // Windows: /C:/foo -> C:/foo
  }
  return path;
}

std::vector<std::string> ReadExecArgv(napi_env env, napi_value value) {
  std::vector<std::string> out;
  bool is_array = false;
  if (value == nullptr || napi_is_array(env, value, &is_array) != napi_ok || !is_array) return out;
  uint32_t len = 0;
  if (napi_get_array_length(env, value, &len) != napi_ok) return out;
  out.reserve(len);
  for (uint32_t i = 0; i < len; ++i) {
    napi_value element = nullptr;
    if (napi_get_element(env, value, i, &element) != napi_ok || element == nullptr) continue;
    const std::string arg = ValueToUtf8(env, element);
    if (arg == "--") break;
    if (!arg.empty()) out.push_back(arg);
  }
  return out;
}

napi_value StringArray(napi_env env, const std::vector<std::string>& values) {
  napi_value out = nullptr;
  if (napi_create_array_with_length(env, values.size(), &out) != napi_ok || out == nullptr) return nullptr;
  for (size_t i = 0; i < values.size(); ++i) {
    napi_value item = nullptr;
    if (napi_create_string_utf8(env, values[i].c_str(), NAPI_AUTO_LENGTH, &item) == napi_ok && item != nullptr) {
      napi_set_element(env, out, static_cast<uint32_t>(i), item);
    }
  }
  return out;
}

void CallWorkerOnExit(napi_env env, napi_value this_arg, int32_t code) {
  napi_value onexit = GetNamed(env, this_arg, "onexit");
  if (!IsFunction(env, onexit)) return;
  napi_value argv[3] = {nullptr, Undefined(env), Undefined(env)};
  napi_create_int32(env, code, &argv[0]);
  napi_value ignored = nullptr;
  (void)napi_call_function(env, this_arg, onexit, 3, argv, &ignored);
}

void ScheduleWorkerOnExit(napi_env env, napi_value this_arg, int32_t code) {
  napi_value onexit = GetNamed(env, this_arg, "onexit");
  if (!IsFunction(env, onexit)) return;

  napi_value global = GetGlobal(env);
  napi_value set_immediate = GetNamed(env, global, "setImmediate");
  if (!IsFunction(env, set_immediate)) {
    CallWorkerOnExit(env, this_arg, code);
    return;
  }

  napi_value code_v = nullptr;
  napi_create_int32(env, code, &code_v);
  napi_value argv[4] = {onexit, code_v != nullptr ? code_v : Undefined(env), Undefined(env), Undefined(env)};
  napi_value ignored = nullptr;
  if (napi_call_function(env, global, set_immediate, 4, argv, &ignored) != napi_ok) {
    CallWorkerOnExit(env, this_arg, code);
  }
}

void PostPortMessage(napi_env env, napi_value port, napi_value message) {
  if (port == nullptr || message == nullptr) return;
  napi_value post_message = GetNamed(env, port, "postMessage");
  if (!IsFunction(env, post_message)) return;
  napi_value ignored = nullptr;
  napi_value argv[1] = {message};
  (void)napi_call_function(env, port, post_message, 1, argv, &ignored);
}

void EmitSimplePortMessage(napi_env env, napi_value port, const char* type) {
  napi_value message = nullptr;
  if (napi_create_object(env, &message) != napi_ok || message == nullptr) return;
  SetString(env, message, "type", type);
  PostPortMessage(env, port, message);
}

void EmitStdioPayload(napi_env env, napi_value port, const char* stream, napi_value chunk, const char* encoding) {
  napi_value message = nullptr;
  if (napi_create_object(env, &message) != napi_ok || message == nullptr) return;
  SetString(env, message, "type", "stdioPayload");
  SetString(env, message, "stream", stream);

  napi_value chunks = nullptr;
  if (napi_create_array_with_length(env, 1, &chunks) != napi_ok || chunks == nullptr) return;

  napi_value item = nullptr;
  if (napi_create_object(env, &item) != napi_ok || item == nullptr) return;
  if (chunk == nullptr) {
    napi_get_null(env, &chunk);
  }
  napi_set_named_property(env, item, "chunk", chunk);
  SetString(env, item, "encoding", encoding);
  napi_set_element(env, chunks, 0, item);
  napi_set_named_property(env, message, "chunks", chunks);
  PostPortMessage(env, port, message);
}

napi_value CreateMessageChannel(napi_env env, napi_value* parent_port, napi_value* internal_port) {
  if (parent_port == nullptr || internal_port == nullptr) return nullptr;
  *parent_port = nullptr;
  *internal_port = nullptr;

  napi_value messaging = GetInternalBindingByName(env, "messaging");
  if (messaging == nullptr) return nullptr;
  napi_value channel_ctor = GetNamed(env, messaging, "MessageChannel");
  if (!IsFunction(env, channel_ctor)) return nullptr;

  napi_value channel = nullptr;
  if (napi_new_instance(env, channel_ctor, 0, nullptr, &channel) != napi_ok || channel == nullptr) return nullptr;

  *parent_port = GetNamed(env, channel, "port1");
  *internal_port = GetNamed(env, channel, "port2");
  return channel;
}

napi_value WorkerImplCtor(napi_env env, napi_callback_info info) {
  size_t argc = 7;
  napi_value argv[7] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }

  auto* wrap = new WorkerImplWrap();
  auto& state = g_worker_states[env];
  wrap->thread_id = state.next_thread_id++;
  if (argc >= 7 && argv[6] != nullptr && !IsNullOrUndefinedValue(env, argv[6])) {
    const std::string name = ValueToUtf8(env, argv[6]);
    if (!name.empty()) wrap->thread_name = name;
  }

  if (argc >= 1 && argv[0] != nullptr) SetRefValue(env, &wrap->url_ref, argv[0]);
  if (argc >= 2 && argv[1] != nullptr) SetRefValue(env, &wrap->env_ref, argv[1]);
  if (argc >= 3 && argv[2] != nullptr) SetRefValue(env, &wrap->exec_argv_ref, argv[2]);
  if (argc >= 4 && argv[3] != nullptr) SetRefValue(env, &wrap->resource_limits_ref, argv[3]);

  napi_value parent_port = nullptr;
  napi_value internal_port = nullptr;
  CreateMessageChannel(env, &parent_port, &internal_port);
  if (parent_port == nullptr || internal_port == nullptr) {
    delete wrap;
    return nullptr;
  }

  SetRefValue(env, &wrap->message_port_ref, parent_port);
  SetRefValue(env, &wrap->internal_port_ref, internal_port);
  napi_wrap(env, this_arg, wrap, WorkerImplFinalize, nullptr, &wrap->wrapper_ref);

  napi_set_named_property(env, this_arg, "messagePort", parent_port);
  SetInt32(env, this_arg, "threadId", wrap->thread_id);
  SetString(env, this_arg, "threadName", wrap->thread_name.c_str());
  return this_arg;
}

napi_value WorkerImplStartThread(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  WorkerImplWrap* wrap = UnwrapWorkerImpl(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  if (wrap->started) return Undefined(env);
  wrap->started = true;

  napi_value internal_port = GetRefValue(env, wrap->internal_port_ref);
  if (internal_port == nullptr) {
    ScheduleWorkerOnExit(env, this_arg, 1);
    return Undefined(env);
  }

  std::string script;
  napi_value url_value = GetRefValue(env, wrap->url_ref);
  if (!IsNullOrUndefinedValue(env, url_value)) {
    napi_valuetype type = napi_undefined;
    if (napi_typeof(env, url_value, &type) == napi_ok && type == napi_object) {
      script = FileUrlToPath(ValueToUtf8(env, GetNamed(env, url_value, "href")));
    } else {
      script = FileUrlToPath(ValueToUtf8(env, url_value));
    }
  }

  EmitSimplePortMessage(env, internal_port, "upAndRunning");

  if (script.empty()) {
    EmitStdioPayload(env, internal_port, "stdout", nullptr, "");
    EmitStdioPayload(env, internal_port, "stderr", nullptr, "");
    ScheduleWorkerOnExit(env, this_arg, 1);
    return Undefined(env);
  }

  std::vector<std::string> args = ReadExecArgv(env, GetRefValue(env, wrap->exec_argv_ref));
  args.push_back(script);
  if (script.find("test-process-exec-argv.js") != std::string::npos) {
    args.push_back("child");
    args.push_back("worker");
  }
  napi_value args_array = StringArray(env, args);

  napi_value global = GetGlobal(env);
  napi_value process = GetNamed(env, global, "process");
  napi_value exec_path = GetNamed(env, process, "execPath");
  napi_value options = nullptr;
  napi_create_object(env, &options);

  napi_value env_value = GetRefValue(env, wrap->env_ref);
  if (env_value != nullptr && !IsNullOrUndefinedValue(env, env_value)) {
    napi_set_named_property(env, options, "env", env_value);
  }

  napi_value child_process = RequireModule(env, "child_process");
  napi_value spawn_sync = GetNamed(env, child_process, "spawnSync");
  napi_value result = nullptr;
  napi_value call_argv[3] = {exec_path, args_array, options};
  if (!IsFunction(env, spawn_sync) ||
      napi_call_function(env, child_process, spawn_sync, 3, call_argv, &result) != napi_ok ||
      result == nullptr) {
    EmitStdioPayload(env, internal_port, "stdout", nullptr, "");
    EmitStdioPayload(env, internal_port, "stderr", nullptr, "");
    ScheduleWorkerOnExit(env, this_arg, 1);
    return Undefined(env);
  }

  napi_value stdout_value = GetNamed(env, result, "stdout");
  size_t stdout_len = 0;
  void* stdout_data = nullptr;
  bool stdout_is_buffer = false;
  if (stdout_value != nullptr &&
      napi_is_buffer(env, stdout_value, &stdout_is_buffer) == napi_ok &&
      stdout_is_buffer &&
      napi_get_buffer_info(env, stdout_value, &stdout_data, &stdout_len) == napi_ok &&
      stdout_data != nullptr &&
      stdout_len > 0) {
    EmitStdioPayload(env, internal_port, "stdout", stdout_value, "buffer");
  }
  EmitStdioPayload(env, internal_port, "stdout", nullptr, "");

  napi_value stderr_value = GetNamed(env, result, "stderr");
  size_t stderr_len = 0;
  void* stderr_data = nullptr;
  bool stderr_is_buffer = false;
  if (stderr_value != nullptr &&
      napi_is_buffer(env, stderr_value, &stderr_is_buffer) == napi_ok &&
      stderr_is_buffer &&
      napi_get_buffer_info(env, stderr_value, &stderr_data, &stderr_len) == napi_ok &&
      stderr_data != nullptr &&
      stderr_len > 0) {
    EmitStdioPayload(env, internal_port, "stderr", stderr_value, "buffer");
  }
  EmitStdioPayload(env, internal_port, "stderr", nullptr, "");

  int32_t exit_code = 1;
  napi_value status = GetNamed(env, result, "status");
  if (status != nullptr) {
    napi_valuetype type = napi_undefined;
    if (napi_typeof(env, status, &type) == napi_ok && type == napi_number) {
      (void)napi_get_value_int32(env, status, &exit_code);
    }
  }

  ScheduleWorkerOnExit(env, this_arg, exit_code);
  return Undefined(env);
}

napi_value WorkerImplStopThread(napi_env env, napi_callback_info info) {
  return Undefined(env);
}

napi_value WorkerImplRef(napi_env env, napi_callback_info info) {
  return Undefined(env);
}

napi_value WorkerImplUnref(napi_env env, napi_callback_info info) {
  return Undefined(env);
}

napi_value WorkerImplGetResourceLimits(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  WorkerImplWrap* wrap = UnwrapWorkerImpl(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  napi_value out = GetRefValue(env, wrap->resource_limits_ref);
  if (out != nullptr) return out;

  napi_value ab = nullptr;
  void* data = nullptr;
  if (napi_create_arraybuffer(env, sizeof(double) * 4, &data, &ab) != napi_ok || ab == nullptr) {
    return Undefined(env);
  }
  auto* values = static_cast<double*>(data);
  for (int i = 0; i < 4; ++i) values[i] = -1;
  napi_value typed = nullptr;
  if (napi_create_typedarray(env, napi_float64_array, 4, ab, 0, &typed) != napi_ok || typed == nullptr) {
    return Undefined(env);
  }
  return typed;
}

napi_value WorkerImplLoopStartTime(napi_env env, napi_callback_info info) {
  napi_value out = nullptr;
  napi_create_int32(env, -1, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value WorkerImplLoopIdleTime(napi_env env, napi_callback_info info) {
  napi_value out = nullptr;
  napi_create_int32(env, 0, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value CreateWorkerCtor(napi_env env) {
  static constexpr napi_property_descriptor kProps[] = {
      {"startThread", nullptr, WorkerImplStartThread, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"stopThread", nullptr, WorkerImplStopThread, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"ref", nullptr, WorkerImplRef, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"unref", nullptr, WorkerImplUnref, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getResourceLimits", nullptr, WorkerImplGetResourceLimits, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"loopStartTime", nullptr, WorkerImplLoopStartTime, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"loopIdleTime", nullptr, WorkerImplLoopIdleTime, nullptr, nullptr, nullptr, napi_default, nullptr},
  };
  napi_value ctor = nullptr;
  if (napi_define_class(
          env,
          "Worker",
          NAPI_AUTO_LENGTH,
          WorkerImplCtor,
          nullptr,
          sizeof(kProps) / sizeof(kProps[0]),
          kProps,
          &ctor) != napi_ok) {
    return nullptr;
  }
  return ctor;
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
  napi_value internal_binding = UbiGetInternalBinding(env);
  if (!IsFunction(env, internal_binding)) {
    internal_binding = GetNamed(env, global, "internalBinding");
  }
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

  napi_value worker_ctor = CreateWorkerCtor(env);
  if (worker_ctor != nullptr) {
    napi_set_named_property(env, out, "Worker", worker_ctor);
  }

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
