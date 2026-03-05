#include "internal_binding/dispatch.h"

#include <string>
#include <unordered_map>

#include "internal_binding/helpers.h"

namespace internal_binding {

namespace {

enum ModuleWrapStatus : int32_t {
  kUninstantiated = 0,
  kInstantiating = 1,
  kInstantiated = 2,
  kEvaluating = 3,
  kEvaluated = 4,
  kErrored = 5,
};

constexpr int32_t kSourcePhase = 1;
constexpr int32_t kEvaluationPhase = 2;

struct ModuleWrapInstance {
  napi_ref wrapper_ref = nullptr;
  napi_ref namespace_ref = nullptr;
  napi_ref source_object_ref = nullptr;
  napi_ref synthetic_eval_steps_ref = nullptr;
  napi_ref linker_ref = nullptr;
  napi_ref error_ref = nullptr;
  int32_t status = kUninstantiated;
  int32_t phase = kEvaluationPhase;
  bool has_top_level_await = false;
};

struct ModuleWrapBindingState {
  napi_ref binding_ref = nullptr;
  napi_ref module_wrap_ctor_ref = nullptr;
  napi_ref import_module_dynamically_ref = nullptr;
  napi_ref initialize_import_meta_ref = nullptr;
};

std::unordered_map<napi_env, ModuleWrapBindingState> g_module_wrap_states;

ModuleWrapBindingState* GetBindingState(napi_env env) {
  auto it = g_module_wrap_states.find(env);
  if (it == g_module_wrap_states.end()) return nullptr;
  return &it->second;
}

napi_value GetRefValue(napi_env env, napi_ref ref) {
  if (ref == nullptr) return nullptr;
  napi_value out = nullptr;
  if (napi_get_reference_value(env, ref, &out) != napi_ok || out == nullptr) return nullptr;
  return out;
}

void ResetRef(napi_env env, napi_ref* ref_ptr) {
  if (ref_ptr == nullptr || *ref_ptr == nullptr) return;
  napi_delete_reference(env, *ref_ptr);
  *ref_ptr = nullptr;
}

void SetRef(napi_env env, napi_ref* ref_ptr, napi_value value, napi_valuetype required) {
  if (ref_ptr == nullptr) return;
  ResetRef(env, ref_ptr);
  if (value == nullptr) return;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok || type != required) return;
  napi_create_reference(env, value, 1, ref_ptr);
}

ModuleWrapInstance* UnwrapModuleWrap(napi_env env, napi_value this_arg) {
  if (this_arg == nullptr) return nullptr;
  void* data = nullptr;
  if (napi_unwrap(env, this_arg, &data) != napi_ok || data == nullptr) return nullptr;
  return static_cast<ModuleWrapInstance*>(data);
}

void ModuleWrapFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* instance = static_cast<ModuleWrapInstance*>(data);
  if (instance == nullptr) return;
  ResetRef(env, &instance->wrapper_ref);
  ResetRef(env, &instance->namespace_ref);
  ResetRef(env, &instance->source_object_ref);
  ResetRef(env, &instance->synthetic_eval_steps_ref);
  ResetRef(env, &instance->linker_ref);
  ResetRef(env, &instance->error_ref);
  delete instance;
}

void SetNamedInt(napi_env env, napi_value obj, const char* key, int32_t value) {
  napi_value out = nullptr;
  if (napi_create_int32(env, value, &out) == napi_ok && out != nullptr) {
    napi_set_named_property(env, obj, key, out);
  }
}

void SetNamedMethod(napi_env env, napi_value obj, const char* key, napi_callback cb) {
  napi_value fn = nullptr;
  if (napi_create_function(env, key, NAPI_AUTO_LENGTH, cb, nullptr, &fn) == napi_ok && fn != nullptr) {
    napi_set_named_property(env, obj, key, fn);
  }
}

bool IsFunctionValue(napi_env env, napi_value value) {
  if (value == nullptr) return false;
  napi_valuetype type = napi_undefined;
  return napi_typeof(env, value, &type) == napi_ok && type == napi_function;
}

std::string ValueToUtf8(napi_env env, napi_value value) {
  if (value == nullptr) return {};
  napi_value str = nullptr;
  if (napi_coerce_to_string(env, value, &str) != napi_ok || str == nullptr) return {};
  size_t len = 0;
  if (napi_get_value_string_utf8(env, str, nullptr, 0, &len) != napi_ok) return {};
  std::string out(len + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, str, out.data(), out.size(), &copied) != napi_ok) return {};
  out.resize(copied);
  return out;
}

bool StartsWithNodeScheme(const std::string& specifier) {
  return specifier.rfind("node:", 0) == 0;
}

napi_value CreateCjsNamespaceFromRequire(napi_env env, const std::string& specifier) {
  napi_value global = GetGlobal(env);
  if (global == nullptr) return nullptr;

  napi_value require_fn = nullptr;
  if (napi_get_named_property(env, global, "require", &require_fn) != napi_ok || !IsFunctionValue(env, require_fn)) {
    return nullptr;
  }

  napi_value specifier_value = nullptr;
  if (napi_create_string_utf8(env, specifier.c_str(), NAPI_AUTO_LENGTH, &specifier_value) != napi_ok ||
      specifier_value == nullptr) {
    return nullptr;
  }

  napi_value exports_value = nullptr;
  if (napi_call_function(env, global, require_fn, 1, &specifier_value, &exports_value) != napi_ok ||
      exports_value == nullptr) {
    return nullptr;
  }

  napi_value namespace_obj = nullptr;
  if (napi_create_object(env, &namespace_obj) != napi_ok || namespace_obj == nullptr) {
    return nullptr;
  }
  napi_set_named_property(env, namespace_obj, "default", exports_value);

  napi_valuetype exports_type = napi_undefined;
  if (napi_typeof(env, exports_value, &exports_type) != napi_ok ||
      (exports_type != napi_object && exports_type != napi_function)) {
    return namespace_obj;
  }

  napi_value keys = nullptr;
  if (napi_get_property_names(env, exports_value, &keys) != napi_ok || keys == nullptr) {
    return namespace_obj;
  }
  uint32_t key_count = 0;
  if (napi_get_array_length(env, keys, &key_count) != napi_ok) {
    return namespace_obj;
  }
  for (uint32_t i = 0; i < key_count; ++i) {
    napi_value key = nullptr;
    if (napi_get_element(env, keys, i, &key) != napi_ok || key == nullptr) continue;
    const std::string key_utf8 = ValueToUtf8(env, key);
    if (key_utf8.empty() || key_utf8 == "default") continue;
    napi_value value = nullptr;
    if (napi_get_property(env, exports_value, key, &value) != napi_ok || value == nullptr) continue;
    napi_set_property(env, namespace_obj, key, value);
  }
  return namespace_obj;
}

napi_value GetVmDynamicImportDefaultInternalSymbol(napi_env env) {
  napi_value global = GetGlobal(env);
  if (global == nullptr) return Undefined(env);

  napi_value internal_binding = nullptr;
  if (napi_get_named_property(env, global, "internalBinding", &internal_binding) != napi_ok ||
      !IsFunctionValue(env, internal_binding)) {
    return Undefined(env);
  }

  napi_value symbols_name = nullptr;
  if (napi_create_string_utf8(env, "symbols", NAPI_AUTO_LENGTH, &symbols_name) != napi_ok ||
      symbols_name == nullptr) {
    return Undefined(env);
  }

  napi_value symbols_binding = nullptr;
  if (napi_call_function(env, global, internal_binding, 1, &symbols_name, &symbols_binding) != napi_ok ||
      symbols_binding == nullptr) {
    return Undefined(env);
  }

  napi_value symbol_value = nullptr;
  if (napi_get_named_property(
          env, symbols_binding, "vm_dynamic_import_default_internal", &symbol_value) != napi_ok ||
      symbol_value == nullptr) {
    return Undefined(env);
  }
  return symbol_value;
}

napi_value ModuleWrapCtor(napi_env env, napi_callback_info info) {
  size_t argc = 6;
  napi_value argv[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }

  auto* instance = new ModuleWrapInstance();

  napi_value namespace_obj = nullptr;
  if (napi_create_object(env, &namespace_obj) == napi_ok && namespace_obj != nullptr) {
    napi_create_reference(env, namespace_obj, 1, &instance->namespace_ref);
  }

  const bool has_exports_array = argc >= 3 && argv[2] != nullptr;
  bool is_array = false;
  if (has_exports_array && napi_is_array(env, argv[2], &is_array) == napi_ok && is_array) {
    if (argc >= 4 && argv[3] != nullptr) {
      napi_valuetype t = napi_undefined;
      if (napi_typeof(env, argv[3], &t) == napi_ok && t == napi_function) {
        napi_create_reference(env, argv[3], 1, &instance->synthetic_eval_steps_ref);
      }
    }
    uint32_t exports_len = 0;
    napi_get_array_length(env, argv[2], &exports_len);
    for (uint32_t i = 0; i < exports_len; ++i) {
      napi_value export_name = nullptr;
      if (napi_get_element(env, argv[2], i, &export_name) != napi_ok || export_name == nullptr) continue;
      napi_value undefined = Undefined(env);
      napi_set_property(env, namespace_obj, export_name, undefined);
    }
  }

  napi_wrap(env, this_arg, instance, ModuleWrapFinalize, nullptr, &instance->wrapper_ref);
  return this_arg;
}

napi_value ModuleWrapLink(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  ModuleWrapInstance* instance = UnwrapModuleWrap(env, this_arg);
  if (instance == nullptr) return Undefined(env);

  if (argc >= 1) SetRef(env, &instance->linker_ref, argv[0], napi_function);
  instance->status = kInstantiating;
  return Undefined(env);
}

napi_value ModuleWrapGetModuleRequests(napi_env env, napi_callback_info /*info*/) {
  napi_value out = nullptr;
  napi_create_array_with_length(env, 0, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value ModuleWrapInstantiate(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  ModuleWrapInstance* instance = UnwrapModuleWrap(env, this_arg);
  if (instance != nullptr && instance->status <= kInstantiating) instance->status = kInstantiated;
  return Undefined(env);
}

napi_value ModuleWrapInstantiateSync(napi_env env, napi_callback_info info) {
  return ModuleWrapInstantiate(env, info);
}

napi_value ModuleWrapEvaluateSync(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  ModuleWrapInstance* instance = UnwrapModuleWrap(env, this_arg);
  if (instance == nullptr) return Undefined(env);

  instance->status = kEvaluating;
  if (instance->synthetic_eval_steps_ref != nullptr) {
    napi_value fn = GetRefValue(env, instance->synthetic_eval_steps_ref);
    if (fn != nullptr) {
      napi_value global = GetGlobal(env);
      napi_value ignored = nullptr;
      if (napi_call_function(env, global, fn, 0, nullptr, &ignored) != napi_ok) {
        bool pending = false;
        if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
          napi_value err = nullptr;
          napi_get_and_clear_last_exception(env, &err);
          ResetRef(env, &instance->error_ref);
          if (err != nullptr) napi_create_reference(env, err, 1, &instance->error_ref);
          instance->status = kErrored;
          napi_throw(env, err);
          return nullptr;
        }
      }
    }
  }
  instance->status = kEvaluated;
  return Undefined(env);
}

napi_value ModuleWrapEvaluate(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  ModuleWrapInstance* instance = UnwrapModuleWrap(env, this_arg);
  if (instance == nullptr) return Undefined(env);

  napi_deferred deferred = nullptr;
  napi_value promise = nullptr;
  if (napi_create_promise(env, &deferred, &promise) != napi_ok || promise == nullptr) return Undefined(env);

  instance->status = kEvaluating;
  bool failed = false;
  napi_value err = nullptr;
  if (instance->synthetic_eval_steps_ref != nullptr) {
    napi_value fn = GetRefValue(env, instance->synthetic_eval_steps_ref);
    if (fn != nullptr) {
      napi_value global = GetGlobal(env);
      napi_value ignored = nullptr;
      if (napi_call_function(env, global, fn, 0, nullptr, &ignored) != napi_ok) {
        bool pending = false;
        if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
          napi_get_and_clear_last_exception(env, &err);
          failed = true;
        }
      }
    }
  }

  if (failed) {
    ResetRef(env, &instance->error_ref);
    if (err != nullptr) napi_create_reference(env, err, 1, &instance->error_ref);
    instance->status = kErrored;
    napi_reject_deferred(env, deferred, err != nullptr ? err : Undefined(env));
    return promise;
  }

  instance->status = kEvaluated;
  napi_resolve_deferred(env, deferred, Undefined(env));
  return promise;
}

napi_value ModuleWrapSetExport(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  ModuleWrapInstance* instance = UnwrapModuleWrap(env, this_arg);
  if (instance == nullptr || argc < 1 || argv[0] == nullptr) return Undefined(env);
  napi_value namespace_obj = GetRefValue(env, instance->namespace_ref);
  if (namespace_obj == nullptr) return Undefined(env);
  napi_value value = argc >= 2 && argv[1] != nullptr ? argv[1] : Undefined(env);
  napi_set_property(env, namespace_obj, argv[0], value);
  return Undefined(env);
}

napi_value ModuleWrapSetModuleSourceObject(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  ModuleWrapInstance* instance = UnwrapModuleWrap(env, this_arg);
  if (instance == nullptr) return Undefined(env);
  ResetRef(env, &instance->source_object_ref);
  if (argc >= 1 && argv[0] != nullptr) napi_create_reference(env, argv[0], 1, &instance->source_object_ref);
  instance->phase = kSourcePhase;
  return Undefined(env);
}

napi_value ModuleWrapGetModuleSourceObject(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  ModuleWrapInstance* instance = UnwrapModuleWrap(env, this_arg);
  if (instance == nullptr) return Undefined(env);
  napi_value out = GetRefValue(env, instance->source_object_ref);
  return out != nullptr ? out : Undefined(env);
}

napi_value ModuleWrapCreateCachedData(napi_env env, napi_callback_info info) {
  napi_value arraybuffer = nullptr;
  void* data = nullptr;
  if (napi_create_arraybuffer(env, 0, &data, &arraybuffer) != napi_ok || arraybuffer == nullptr) {
    return Undefined(env);
  }
  napi_value typed_array = nullptr;
  if (napi_create_typedarray(env, napi_uint8_array, 0, arraybuffer, 0, &typed_array) != napi_ok ||
      typed_array == nullptr) {
    return Undefined(env);
  }
  return typed_array;
}

napi_value ModuleWrapGetNamespace(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  ModuleWrapInstance* instance = UnwrapModuleWrap(env, this_arg);
  if (instance == nullptr) return Undefined(env);
  napi_value out = GetRefValue(env, instance->namespace_ref);
  return out != nullptr ? out : Undefined(env);
}

napi_value ModuleWrapGetNamespaceSync(napi_env env, napi_callback_info info) {
  return ModuleWrapGetNamespace(env, info);
}

napi_value ModuleWrapGetStatus(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  ModuleWrapInstance* instance = UnwrapModuleWrap(env, this_arg);
  if (instance == nullptr) return Undefined(env);
  napi_value out = nullptr;
  napi_create_int32(env, instance->status, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value ModuleWrapGetError(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  ModuleWrapInstance* instance = UnwrapModuleWrap(env, this_arg);
  if (instance == nullptr) return Undefined(env);
  napi_value out = GetRefValue(env, instance->error_ref);
  return out != nullptr ? out : Undefined(env);
}

napi_value ModuleWrapHasTopLevelAwait(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  ModuleWrapInstance* instance = UnwrapModuleWrap(env, this_arg);
  bool value = instance != nullptr && instance->has_top_level_await;
  napi_value out = nullptr;
  napi_get_boolean(env, value, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value ModuleWrapIsGraphAsync(napi_env env, napi_callback_info /*info*/) {
  napi_value out = nullptr;
  napi_get_boolean(env, false, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value ModuleWrapSetImportModuleDynamicallyCallback(napi_env env, napi_callback_info info) {
  auto* state = GetBindingState(env);
  if (state == nullptr) return Undefined(env);
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  SetRef(env, &state->import_module_dynamically_ref, argc >= 1 ? argv[0] : nullptr, napi_function);
  return Undefined(env);
}

napi_value ModuleWrapSetInitializeImportMetaObjectCallback(napi_env env, napi_callback_info info) {
  auto* state = GetBindingState(env);
  if (state == nullptr) return Undefined(env);
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  SetRef(env, &state->initialize_import_meta_ref, argc >= 1 ? argv[0] : nullptr, napi_function);
  return Undefined(env);
}

napi_value ModuleWrapImportModuleDynamically(napi_env env, napi_callback_info info) {
  auto* state = GetBindingState(env);
  if (state == nullptr) return Undefined(env);

  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1 || argv[0] == nullptr) return Undefined(env);
  const std::string specifier = ValueToUtf8(env, argv[0]);
  const bool is_node_builtin = StartsWithNodeScheme(specifier);

  if (is_node_builtin) {
    napi_value namespace_obj = CreateCjsNamespaceFromRequire(env, specifier);
    if (namespace_obj != nullptr) return namespace_obj;
  }

  napi_value cb = GetRefValue(env, state->import_module_dynamically_ref);
  if (!IsFunctionValue(env, cb)) {
    if (is_node_builtin) {
      napi_value namespace_obj = CreateCjsNamespaceFromRequire(env, specifier);
      if (namespace_obj != nullptr) return namespace_obj;
    }
    napi_throw_error(env, nullptr, "Not supported");
    return nullptr;
  }

  napi_value referrer_symbol = GetVmDynamicImportDefaultInternalSymbol(env);
  napi_value phase = nullptr;
  if (napi_create_int32(env, kEvaluationPhase, &phase) != napi_ok || phase == nullptr) {
    return Undefined(env);
  }
  napi_value attrs = nullptr;
  if (napi_create_object(env, &attrs) != napi_ok || attrs == nullptr) {
    return Undefined(env);
  }
  napi_value referrer_name = (argc >= 2 && argv[1] != nullptr) ? argv[1] : Undefined(env);

  napi_value call_argv[5] = {referrer_symbol, argv[0], phase, attrs, referrer_name};
  napi_value global = GetGlobal(env);
  if (global == nullptr) return Undefined(env);
  napi_value result = nullptr;
  if (napi_call_function(env, global, cb, 5, call_argv, &result) != napi_ok) {
    if (is_node_builtin) {
      bool has_exception = false;
      if (napi_is_exception_pending(env, &has_exception) == napi_ok && has_exception) {
        napi_value ignored = nullptr;
        napi_get_and_clear_last_exception(env, &ignored);
      }
      napi_value namespace_obj = CreateCjsNamespaceFromRequire(env, specifier);
      if (namespace_obj != nullptr) return namespace_obj;
    }
    return nullptr;
  }
  return result != nullptr ? result : Undefined(env);
}

napi_value ModuleWrapCreateRequiredModuleFacade(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  return (argc >= 1 && argv[0] != nullptr) ? argv[0] : Undefined(env);
}

napi_value ModuleWrapThrowIfPromiseRejected(napi_env env, napi_callback_info /*info*/) {
  return Undefined(env);
}

}  // namespace

napi_value ResolveModuleWrap(napi_env env, const ResolveOptions& /*options*/) {
  auto& state = g_module_wrap_states[env];
  if (state.binding_ref != nullptr) {
    napi_value existing = GetRefValue(env, state.binding_ref);
    if (existing != nullptr) return existing;
  }

  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) return Undefined(env);

  SetNamedInt(env, binding, "kUninstantiated", kUninstantiated);
  SetNamedInt(env, binding, "kInstantiating", kInstantiating);
  SetNamedInt(env, binding, "kInstantiated", kInstantiated);
  SetNamedInt(env, binding, "kEvaluating", kEvaluating);
  SetNamedInt(env, binding, "kEvaluated", kEvaluated);
  SetNamedInt(env, binding, "kErrored", kErrored);
  SetNamedInt(env, binding, "kSourcePhase", kSourcePhase);
  SetNamedInt(env, binding, "kEvaluationPhase", kEvaluationPhase);

  napi_property_descriptor proto[] = {
      {"link", nullptr, ModuleWrapLink, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getModuleRequests", nullptr, ModuleWrapGetModuleRequests, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"instantiate", nullptr, ModuleWrapInstantiate, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"instantiateSync", nullptr, ModuleWrapInstantiateSync, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"evaluate", nullptr, ModuleWrapEvaluate, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"evaluateSync", nullptr, ModuleWrapEvaluateSync, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setExport", nullptr, ModuleWrapSetExport, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setModuleSourceObject", nullptr, ModuleWrapSetModuleSourceObject, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"getModuleSourceObject", nullptr, ModuleWrapGetModuleSourceObject, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"createCachedData", nullptr, ModuleWrapCreateCachedData, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getNamespace", nullptr, ModuleWrapGetNamespace, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getNamespaceSync", nullptr, ModuleWrapGetNamespaceSync, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getStatus", nullptr, ModuleWrapGetStatus, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getError", nullptr, ModuleWrapGetError, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"hasTopLevelAwait", nullptr, ModuleWrapHasTopLevelAwait, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"isGraphAsync", nullptr, ModuleWrapIsGraphAsync, nullptr, nullptr, nullptr, napi_default, nullptr},
  };

  napi_value module_wrap_ctor = nullptr;
  if (napi_define_class(env,
                        "ModuleWrap",
                        NAPI_AUTO_LENGTH,
                        ModuleWrapCtor,
                        nullptr,
                        sizeof(proto) / sizeof(proto[0]),
                        proto,
                        &module_wrap_ctor) != napi_ok ||
      module_wrap_ctor == nullptr) {
    return Undefined(env);
  }
  napi_set_named_property(env, binding, "ModuleWrap", module_wrap_ctor);
  napi_create_reference(env, module_wrap_ctor, 1, &state.module_wrap_ctor_ref);

  SetNamedMethod(env, binding, "setImportModuleDynamicallyCallback", ModuleWrapSetImportModuleDynamicallyCallback);
  SetNamedMethod(
      env, binding, "setInitializeImportMetaObjectCallback", ModuleWrapSetInitializeImportMetaObjectCallback);
  SetNamedMethod(env, binding, "importModuleDynamically", ModuleWrapImportModuleDynamically);
  SetNamedMethod(env, binding, "createRequiredModuleFacade", ModuleWrapCreateRequiredModuleFacade);
  SetNamedMethod(env, binding, "throwIfPromiseRejected", ModuleWrapThrowIfPromiseRejected);

  napi_create_reference(env, binding, 1, &state.binding_ref);
  return binding;
}

}  // namespace internal_binding
