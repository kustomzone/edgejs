#include "internal_binding/dispatch.h"

#include <unordered_map>
#include <unordered_set>

#include "internal_binding/helpers.h"
#include "ubi_module_loader.h"
#include "ubi_util.h"

namespace internal_binding {

namespace {

std::unordered_map<napi_env, napi_ref> g_symbols_refs;
std::unordered_set<napi_env> g_symbols_cleanup_hook_registered;

void DeleteRefIfPresent(napi_env env, napi_ref* ref) {
  if (env == nullptr || ref == nullptr || *ref == nullptr) return;
  napi_delete_reference(env, *ref);
  *ref = nullptr;
}

void OnSymbolsEnvCleanup(void* data) {
  napi_env env = static_cast<napi_env>(data);
  g_symbols_cleanup_hook_registered.erase(env);

  auto it = g_symbols_refs.find(env);
  if (it == g_symbols_refs.end()) return;
  DeleteRefIfPresent(env, &it->second);
  g_symbols_refs.erase(it);
}

void EnsureSymbolsCleanupHook(napi_env env) {
  if (env == nullptr) return;
  auto [it, inserted] = g_symbols_cleanup_hook_registered.emplace(env);
  if (!inserted) return;
  if (napi_add_env_cleanup_hook(env, OnSymbolsEnvCleanup, env) != napi_ok) {
    g_symbols_cleanup_hook_registered.erase(it);
  }
}

napi_value GetCachedSymbols(napi_env env) {
  auto it = g_symbols_refs.find(env);
  if (it == g_symbols_refs.end() || it->second == nullptr) return nullptr;
  napi_value out = nullptr;
  if (napi_get_reference_value(env, it->second, &out) != napi_ok || out == nullptr) return nullptr;
  return out;
}

napi_value GetPerIsolateSymbolSource(napi_env env) {
  napi_value source = UbiGetPerIsolateSymbols(env);
  if (source != nullptr && !IsUndefined(env, source)) return source;

  source = UbiCreatePerIsolateSymbolsObject(env);
  if (source != nullptr && !IsUndefined(env, source)) {
    UbiSetPerIsolateSymbols(env, source);
  }
  return source;
}

}  // namespace

napi_value ResolveSymbols(napi_env env, const ResolveOptions& /*options*/) {
  EnsureSymbolsCleanupHook(env);
  const napi_value undefined = Undefined(env);
  napi_value existing = GetCachedSymbols(env);
  if (existing != nullptr) return existing;

  napi_value source = GetPerIsolateSymbolSource(env);
  if (source == nullptr || IsUndefined(env, source)) return undefined;

  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) return undefined;

  auto set_symbol = [&](const char* target_key, const char* source_key) {
    napi_value sym = nullptr;
    if (napi_get_named_property(env, source, source_key, &sym) == napi_ok && sym != nullptr) {
      napi_set_named_property(env, out, target_key, sym);
    }
  };

  set_symbol("vm_dynamic_import_default_internal", "vm_dynamic_import_default_internal");
  set_symbol("vm_dynamic_import_main_context_default", "vm_dynamic_import_main_context_default");
  set_symbol("vm_dynamic_import_no_callback", "vm_dynamic_import_no_callback");
  set_symbol("vm_dynamic_import_missing_flag", "vm_dynamic_import_missing_flag");
  set_symbol("vm_context_no_contextify", "vm_context_no_contextify");
  set_symbol("source_text_module_default_hdo", "source_text_module_default_hdo");
  set_symbol("constructor_key_symbol", "constructor_key_symbol");
  set_symbol("fs_use_promises_symbol", "fs_use_promises_symbol");
  set_symbol("handle_onclose", "handle_onclose_symbol");
  set_symbol("resource_symbol", "resource_symbol");
  set_symbol("owner_symbol", "owner_symbol");
  set_symbol("async_id_symbol", "async_id_symbol");
  set_symbol("trigger_async_id_symbol", "trigger_async_id_symbol");
  set_symbol("oninit", "oninit_symbol");
  set_symbol("onpskexchange", "onpskexchange_symbol");
  set_symbol("messaging_deserialize_symbol", "messaging_deserialize_symbol");
  set_symbol("messaging_transfer_symbol", "messaging_transfer_symbol");
  set_symbol("messaging_clone_symbol", "messaging_clone_symbol");
  set_symbol("messaging_transfer_list_symbol", "messaging_transfer_list_symbol");
  set_symbol("no_message_symbol", "no_message_symbol");
  set_symbol("imported_cjs_symbol", "imported_cjs_symbol");

  auto& ref = g_symbols_refs[env];
  DeleteRefIfPresent(env, &ref);
  napi_create_reference(env, out, 1, &ref);
  return out;
}

}  // namespace internal_binding
