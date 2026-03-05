#include "internal_binding/dispatch.h"

#include <unordered_map>

#include "internal_binding/helpers.h"

namespace internal_binding {

namespace {

std::unordered_map<napi_env, napi_ref> g_permission_refs;

napi_value HasPermissionCallback(napi_env env, napi_callback_info /*info*/) {
  napi_value out = nullptr;
  napi_get_boolean(env, false, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value GetCachedPermission(napi_env env) {
  auto it = g_permission_refs.find(env);
  if (it == g_permission_refs.end() || it->second == nullptr) return nullptr;
  napi_value out = nullptr;
  if (napi_get_reference_value(env, it->second, &out) != napi_ok || out == nullptr) return nullptr;
  return out;
}

}  // namespace

napi_value ResolvePermission(napi_env env, const ResolveOptions& /*options*/) {
  const napi_value undefined = Undefined(env);
  napi_value cached = GetCachedPermission(env);
  if (cached != nullptr) return cached;

  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) return undefined;

  napi_value has_fn = nullptr;
  if (napi_create_function(env,
                           "has",
                           NAPI_AUTO_LENGTH,
                           HasPermissionCallback,
                           nullptr,
                           &has_fn) == napi_ok &&
      has_fn != nullptr) {
    napi_set_named_property(env, out, "has", has_fn);
  }

  auto& ref = g_permission_refs[env];
  if (ref != nullptr) {
    napi_delete_reference(env, ref);
    ref = nullptr;
  }
  napi_create_reference(env, out, 1, &ref);
  return out;
}

}  // namespace internal_binding
