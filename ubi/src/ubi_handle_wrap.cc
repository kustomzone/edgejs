#include "ubi_handle_wrap.h"

#include <unordered_map>
#include <unordered_set>

#include "internal_binding/helpers.h"
#include "ubi_module_loader.h"
#include "ubi_runtime.h"

namespace {

struct HandleSymbolCache {
  napi_ref symbols_ref = nullptr;
  napi_ref owner_symbol_ref = nullptr;
  napi_ref handle_onclose_symbol_ref = nullptr;
};

std::unordered_map<napi_env, HandleSymbolCache> g_handle_symbols;
std::unordered_set<napi_env> g_handle_symbol_cleanup_hook_registered;

void DeleteRefIfPresent(napi_env env, napi_ref* ref) {
  if (env == nullptr || ref == nullptr || *ref == nullptr) return;
  napi_delete_reference(env, *ref);
  *ref = nullptr;
}

void OnHandleSymbolsEnvCleanup(void* data) {
  napi_env env = static_cast<napi_env>(data);
  g_handle_symbol_cleanup_hook_registered.erase(env);

  auto it = g_handle_symbols.find(env);
  if (it == g_handle_symbols.end()) return;
  DeleteRefIfPresent(env, &it->second.symbols_ref);
  DeleteRefIfPresent(env, &it->second.owner_symbol_ref);
  DeleteRefIfPresent(env, &it->second.handle_onclose_symbol_ref);
  g_handle_symbols.erase(it);
}

void EnsureHandleSymbolsCleanupHook(napi_env env) {
  if (env == nullptr) return;
  auto [it, inserted] = g_handle_symbol_cleanup_hook_registered.emplace(env);
  if (!inserted) return;
  if (napi_add_env_cleanup_hook(env, OnHandleSymbolsEnvCleanup, env) != napi_ok) {
    g_handle_symbol_cleanup_hook_registered.erase(it);
  }
}

napi_value ResolveInternalBinding(napi_env env, const char* name) {
  if (env == nullptr || name == nullptr) return nullptr;

  napi_value global = internal_binding::GetGlobal(env);
  if (global == nullptr) return nullptr;

  napi_value internal_binding = UbiGetInternalBinding(env);
  if (internal_binding == nullptr) {
    if (napi_get_named_property(env, global, "internalBinding", &internal_binding) != napi_ok ||
        internal_binding == nullptr) {
      return nullptr;
    }
  }

  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, internal_binding, &type) != napi_ok || type != napi_function) {
    return nullptr;
  }

  napi_value binding_name = nullptr;
  if (napi_create_string_utf8(env, name, NAPI_AUTO_LENGTH, &binding_name) != napi_ok ||
      binding_name == nullptr) {
    return nullptr;
  }

  napi_value binding = nullptr;
  napi_value argv[1] = {binding_name};
  if (napi_call_function(env, global, internal_binding, 1, argv, &binding) != napi_ok ||
      binding == nullptr) {
    bool pending = false;
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
      napi_value ignored = nullptr;
      napi_get_and_clear_last_exception(env, &ignored);
    }
    return nullptr;
  }
  return binding;
}

HandleSymbolCache& GetHandleSymbolCache(napi_env env) {
  EnsureHandleSymbolsCleanupHook(env);
  return g_handle_symbols[env];
}

napi_value GetSymbolsBinding(napi_env env) {
  HandleSymbolCache& cache = GetHandleSymbolCache(env);
  napi_value binding = UbiHandleWrapGetRefValue(env, cache.symbols_ref);
  if (binding != nullptr) return binding;

  binding = ResolveInternalBinding(env, "symbols");
  if (binding == nullptr) return nullptr;

  UbiHandleWrapDeleteRefIfPresent(env, &cache.symbols_ref);
  napi_create_reference(env, binding, 1, &cache.symbols_ref);
  return binding;
}

napi_value GetNamedCachedSymbol(napi_env env, const char* key, napi_ref* slot) {
  if (slot == nullptr) return nullptr;
  napi_value symbol = UbiHandleWrapGetRefValue(env, *slot);
  if (symbol != nullptr) return symbol;

  napi_value symbols = GetSymbolsBinding(env);
  if (symbols == nullptr) return nullptr;
  if (napi_get_named_property(env, symbols, key, &symbol) != napi_ok || symbol == nullptr) {
    return nullptr;
  }

  UbiHandleWrapDeleteRefIfPresent(env, slot);
  napi_create_reference(env, symbol, 1, slot);
  return symbol;
}

napi_value GetOwnerSymbol(napi_env env) {
  HandleSymbolCache& cache = GetHandleSymbolCache(env);
  return GetNamedCachedSymbol(env, "owner_symbol", &cache.owner_symbol_ref);
}

napi_value GetHandleOnCloseSymbol(napi_env env) {
  HandleSymbolCache& cache = GetHandleSymbolCache(env);
  return GetNamedCachedSymbol(env, "handle_onclose", &cache.handle_onclose_symbol_ref);
}

void SetPropertyIfPresent(napi_env env, napi_value obj, napi_value key, napi_value value) {
  if (env == nullptr || obj == nullptr || key == nullptr || value == nullptr) return;
  napi_set_property(env, obj, key, value);
}

}  // namespace

void UbiHandleWrapInit(UbiHandleWrap* wrap, napi_env env) {
  if (wrap == nullptr) return;
  wrap->env = env;
  wrap->wrapper_ref = nullptr;
  wrap->active_handle_token = nullptr;
  wrap->finalized = false;
  wrap->delete_on_close = false;
  wrap->wrapper_ref_held = false;
  wrap->state = kUbiHandleUninitialized;
}

napi_value UbiHandleWrapGetRefValue(napi_env env, napi_ref ref) {
  if (env == nullptr || ref == nullptr) return nullptr;
  napi_value value = nullptr;
  if (napi_get_reference_value(env, ref, &value) != napi_ok || value == nullptr) {
    return nullptr;
  }
  return value;
}

void UbiHandleWrapDeleteRefIfPresent(napi_env env, napi_ref* ref) {
  if (env == nullptr || ref == nullptr || *ref == nullptr) return;
  napi_delete_reference(env, *ref);
  *ref = nullptr;
}

void UbiHandleWrapHoldWrapperRef(UbiHandleWrap* wrap) {
  if (wrap == nullptr || wrap->env == nullptr || wrap->wrapper_ref == nullptr || wrap->wrapper_ref_held) return;
  uint32_t ref_count = 0;
  if (napi_reference_ref(wrap->env, wrap->wrapper_ref, &ref_count) == napi_ok) {
    wrap->wrapper_ref_held = true;
  }
}

void UbiHandleWrapReleaseWrapperRef(UbiHandleWrap* wrap) {
  if (wrap == nullptr || wrap->env == nullptr || wrap->wrapper_ref == nullptr || !wrap->wrapper_ref_held) return;
  uint32_t ref_count = 0;
  if (napi_reference_unref(wrap->env, wrap->wrapper_ref, &ref_count) == napi_ok) {
    wrap->wrapper_ref_held = false;
  }
}

napi_value UbiHandleWrapGetActiveOwner(napi_env env, napi_ref wrapper_ref) {
  napi_value wrapper = UbiHandleWrapGetRefValue(env, wrapper_ref);
  if (wrapper == nullptr) return nullptr;

  napi_value owner_symbol = GetOwnerSymbol(env);
  if (owner_symbol != nullptr) {
    napi_value owner = nullptr;
    if (napi_get_property(env, wrapper, owner_symbol, &owner) == napi_ok && owner != nullptr) {
      napi_valuetype type = napi_undefined;
      if (napi_typeof(env, owner, &type) == napi_ok && type != napi_undefined && type != napi_null) {
        return owner;
      }
    }
  }
  return wrapper;
}

void UbiHandleWrapSetOnCloseCallback(napi_env env, napi_value wrapper, napi_value callback) {
  if (env == nullptr || wrapper == nullptr || callback == nullptr) return;
  napi_value symbol = GetHandleOnCloseSymbol(env);
  if (symbol == nullptr) return;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, callback, &type) != napi_ok || type != napi_function) return;
  SetPropertyIfPresent(env, wrapper, symbol, callback);
}

void UbiHandleWrapMaybeCallOnClose(UbiHandleWrap* wrap) {
  if (wrap == nullptr || wrap->env == nullptr || wrap->finalized) return;
  napi_value self = UbiHandleWrapGetRefValue(wrap->env, wrap->wrapper_ref);
  if (self == nullptr) return;

  napi_value symbol = GetHandleOnCloseSymbol(wrap->env);
  if (symbol == nullptr) return;

  bool has_callback = false;
  if (napi_has_property(wrap->env, self, symbol, &has_callback) != napi_ok || !has_callback) {
    return;
  }

  napi_value callback = nullptr;
  if (napi_get_property(wrap->env, self, symbol, &callback) != napi_ok || callback == nullptr) return;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(wrap->env, callback, &type) != napi_ok || type != napi_function) return;

  napi_value ignored = nullptr;
  UbiMakeCallback(wrap->env, self, callback, 0, nullptr, &ignored);

  napi_value undefined = nullptr;
  napi_get_undefined(wrap->env, &undefined);
  SetPropertyIfPresent(wrap->env, self, symbol, undefined);
}

bool UbiHandleWrapHasRef(const UbiHandleWrap* wrap, const uv_handle_t* handle) {
  if (wrap == nullptr || handle == nullptr || wrap->state != kUbiHandleInitialized) return false;
  return uv_has_ref(handle) != 0;
}
