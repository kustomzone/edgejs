#include "internal_binding/dispatch.h"

#include "edge_environment.h"
#include "internal_binding/helpers.h"

namespace internal_binding {

namespace {

void DeleteRefIfPresent(napi_env env, napi_ref* ref) {
  if (env == nullptr || ref == nullptr || *ref == nullptr) return;
  napi_delete_reference(env, *ref);
  *ref = nullptr;
}

struct MksnapshotBindingState {
  explicit MksnapshotBindingState(napi_env env_in) : env(env_in) {}
  ~MksnapshotBindingState() {
    DeleteRefIfPresent(env, &binding_ref);
  }

  napi_env env = nullptr;
  napi_ref binding_ref = nullptr;
};

MksnapshotBindingState* GetMksnapshotState(napi_env env) {
  return EdgeEnvironmentGetSlotData<MksnapshotBindingState>(
      env, kEdgeEnvironmentSlotMksnapshotBindingState);
}

MksnapshotBindingState& EnsureMksnapshotState(napi_env env) {
  return EdgeEnvironmentGetOrCreateSlotData<MksnapshotBindingState>(
      env, kEdgeEnvironmentSlotMksnapshotBindingState);
}

napi_value ReturnUndefined(napi_env env, napi_callback_info /*info*/) {
  return Undefined(env);
}

napi_value GetCachedMksnapshot(napi_env env) {
  auto* state = GetMksnapshotState(env);
  if (state == nullptr || state->binding_ref == nullptr) return nullptr;
  napi_value out = nullptr;
  if (napi_get_reference_value(env, state->binding_ref, &out) != napi_ok || out == nullptr) return nullptr;
  return out;
}

}  // namespace

napi_value ResolveMksnapshot(napi_env env, const ResolveOptions& /*options*/) {
  const napi_value undefined = Undefined(env);
  napi_value cached = GetCachedMksnapshot(env);
  if (cached != nullptr) return cached;

  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) return undefined;

  auto define_noop = [&](const char* name) {
    napi_value fn = nullptr;
    if (napi_create_function(env, name, NAPI_AUTO_LENGTH, ReturnUndefined, nullptr, &fn) == napi_ok &&
        fn != nullptr) {
      napi_set_named_property(env, out, name, fn);
    }
  };
  define_noop("runEmbedderPreload");
  define_noop("compileSerializeMain");
  define_noop("setSerializeCallback");
  define_noop("setDeserializeCallback");
  define_noop("setDeserializeMainFunction");

  napi_value is_building_snapshot_buffer = nullptr;
  napi_value global = nullptr;
  napi_value ctor = nullptr;
  napi_value length = nullptr;
  if (napi_get_global(env, &global) == napi_ok &&
      global != nullptr &&
      napi_get_named_property(env, global, "Uint8Array", &ctor) == napi_ok &&
      ctor != nullptr &&
      napi_create_uint32(env, 1, &length) == napi_ok &&
      length != nullptr) {
    napi_value argv[1] = {length};
    if (napi_new_instance(env, ctor, 1, argv, &is_building_snapshot_buffer) == napi_ok &&
        is_building_snapshot_buffer != nullptr) {
      napi_value zero = nullptr;
      if (napi_get_boolean(env, false, &zero) == napi_ok && zero != nullptr) {
        napi_set_element(env, is_building_snapshot_buffer, 0, zero);
      }
      napi_set_named_property(env, out, "isBuildingSnapshotBuffer", is_building_snapshot_buffer);
    }
  }

  SetString(env, out, "anonymousMainPath", "<anonymous>");

  auto& state = EnsureMksnapshotState(env);
  DeleteRefIfPresent(env, &state.binding_ref);
  napi_create_reference(env, out, 1, &state.binding_ref);
  return out;
}

}  // namespace internal_binding
