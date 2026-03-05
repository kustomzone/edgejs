#include "internal_binding/dispatch.h"

#include <unordered_map>

#include "internal_binding/helpers.h"

namespace internal_binding {

namespace {

struct AsyncContextFrameState {
  napi_ref binding_ref = nullptr;
  napi_ref continuation_frame_ref = nullptr;
};

std::unordered_map<napi_env, AsyncContextFrameState> g_async_context_frame_states;

AsyncContextFrameState& GetState(napi_env env) {
  return g_async_context_frame_states[env];
}

void ReleaseFrameRef(napi_env env, AsyncContextFrameState* state) {
  if (env == nullptr || state == nullptr || state->continuation_frame_ref == nullptr) return;
  napi_delete_reference(env, state->continuation_frame_ref);
  state->continuation_frame_ref = nullptr;
}

void OnAsyncContextFrameEnvCleanup(void* arg) {
  napi_env env = static_cast<napi_env>(arg);
  auto it = g_async_context_frame_states.find(env);
  if (it == g_async_context_frame_states.end()) return;
  ReleaseFrameRef(env, &it->second);
  if (it->second.binding_ref != nullptr) {
    napi_delete_reference(env, it->second.binding_ref);
    it->second.binding_ref = nullptr;
  }
  g_async_context_frame_states.erase(it);
}

void EnsureAsyncContextFrameCleanupHook(napi_env env) {
  static std::unordered_map<napi_env, bool> installed;
  if (installed[env]) return;
  if (napi_add_env_cleanup_hook(env, OnAsyncContextFrameEnvCleanup, env) == napi_ok) {
    installed[env] = true;
  }
}

napi_value GetContinuationPreservedEmbedderData(napi_env env, napi_callback_info info) {
  (void)info;
  AsyncContextFrameState& state = GetState(env);
  if (state.continuation_frame_ref == nullptr) return Undefined(env);
  napi_value value = nullptr;
  if (napi_get_reference_value(env, state.continuation_frame_ref, &value) != napi_ok || value == nullptr) {
    return Undefined(env);
  }
  return value;
}

napi_value SetContinuationPreservedEmbedderData(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  AsyncContextFrameState& state = GetState(env);
  ReleaseFrameRef(env, &state);

  if (argc >= 1 && argv[0] != nullptr && !IsUndefined(env, argv[0])) {
    napi_create_reference(env, argv[0], 1, &state.continuation_frame_ref);
  }
  return Undefined(env);
}

}  // namespace

napi_value ResolveAsyncContextFrame(napi_env env, const ResolveOptions& options) {
  (void)options;
  EnsureAsyncContextFrameCleanupHook(env);
  AsyncContextFrameState& state = GetState(env);
  if (state.binding_ref != nullptr) {
    napi_value cached = nullptr;
    if (napi_get_reference_value(env, state.binding_ref, &cached) == napi_ok && cached != nullptr) {
      return cached;
    }
  }

  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) return Undefined(env);

  napi_value getter = nullptr;
  napi_create_function(env,
                       "getContinuationPreservedEmbedderData",
                       NAPI_AUTO_LENGTH,
                       GetContinuationPreservedEmbedderData,
                       nullptr,
                       &getter);
  if (getter != nullptr) {
    napi_set_named_property(env, binding, "getContinuationPreservedEmbedderData", getter);
  }

  napi_value setter = nullptr;
  napi_create_function(env,
                       "setContinuationPreservedEmbedderData",
                       NAPI_AUTO_LENGTH,
                       SetContinuationPreservedEmbedderData,
                       nullptr,
                       &setter);
  if (setter != nullptr) {
    napi_set_named_property(env, binding, "setContinuationPreservedEmbedderData", setter);
  }

  if (state.binding_ref != nullptr) {
    napi_delete_reference(env, state.binding_ref);
    state.binding_ref = nullptr;
  }
  napi_create_reference(env, binding, 1, &state.binding_ref);

  return binding;
}

}  // namespace internal_binding
