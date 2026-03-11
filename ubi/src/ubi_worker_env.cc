#include "ubi_worker_env.h"

#include <array>
#include <map>
#include <mutex>
#include <unordered_set>
#include <unordered_map>

#include <uv.h>

namespace {

struct WorkerEnvState {
  bool cleanup_hook_registered = false;
  bool stop_requested = false;
  UbiWorkerEnvConfig config;
  napi_ref binding_ref = nullptr;
  napi_ref env_message_port_ref = nullptr;
  std::unordered_set<int> unmanaged_fds;
};

std::mutex g_worker_env_mu;
std::unordered_map<napi_env, WorkerEnvState> g_worker_env_states;

void DeleteRefIfPresent(napi_env env, napi_ref* ref) {
  if (ref == nullptr || *ref == nullptr) return;
  napi_delete_reference(env, *ref);
  *ref = nullptr;
}

napi_value GetRefValue(napi_env env, napi_ref ref) {
  if (ref == nullptr) return nullptr;
  napi_value out = nullptr;
  if (napi_get_reference_value(env, ref, &out) != napi_ok || out == nullptr) return nullptr;
  return out;
}

void SetRefValue(napi_env env, napi_ref* slot, napi_value value) {
  if (slot == nullptr) return;
  DeleteRefIfPresent(env, slot);
  if (value != nullptr) {
    napi_create_reference(env, value, 1, slot);
  }
}

void EmitProcessWarning(napi_env env, const std::string& message) {
  if (env == nullptr || message.empty()) return;
  napi_value global = nullptr;
  napi_value process = nullptr;
  napi_value emit_warning = nullptr;
  napi_value message_value = nullptr;
  if (napi_get_global(env, &global) != napi_ok ||
      global == nullptr ||
      napi_get_named_property(env, global, "process", &process) != napi_ok ||
      process == nullptr ||
      napi_get_named_property(env, process, "emitWarning", &emit_warning) != napi_ok ||
      emit_warning == nullptr ||
      napi_create_string_utf8(env, message.c_str(), NAPI_AUTO_LENGTH, &message_value) != napi_ok ||
      message_value == nullptr) {
    return;
  }
  napi_value ignored = nullptr;
  (void)napi_call_function(env, process, emit_warning, 1, &message_value, &ignored);
}

void CleanupWorkerEnvState(void* data) {
  napi_env env = static_cast<napi_env>(data);
  if (env == nullptr) return;

  std::unordered_set<int> unmanaged_fds;
  {
    std::lock_guard<std::mutex> lock(g_worker_env_mu);
    auto it = g_worker_env_states.find(env);
    if (it == g_worker_env_states.end()) return;
    it->second.cleanup_hook_registered = false;
    it->second.stop_requested = true;
    unmanaged_fds.swap(it->second.unmanaged_fds);
    DeleteRefIfPresent(env, &it->second.binding_ref);
    DeleteRefIfPresent(env, &it->second.env_message_port_ref);
  }

  for (const int fd : unmanaged_fds) {
    if (fd < 0) continue;
    uv_fs_t req{};
    (void)uv_fs_close(nullptr, &req, fd, nullptr);
    uv_fs_req_cleanup(&req);
  }
}

WorkerEnvState& EnsureWorkerEnvState(napi_env env) {
  auto& state = g_worker_env_states[env];
  if (!state.cleanup_hook_registered) {
    if (napi_add_env_cleanup_hook(env, CleanupWorkerEnvState, env) == napi_ok) {
      state.cleanup_hook_registered = true;
    }
  }
  return state;
}

}  // namespace

void UbiWorkerEnvConfigure(napi_env env, const UbiWorkerEnvConfig& config) {
  if (env == nullptr) return;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  auto& state = EnsureWorkerEnvState(env);
  state.config = config;
  state.stop_requested = false;
}

bool UbiWorkerEnvGetConfig(napi_env env, UbiWorkerEnvConfig* out) {
  if (env == nullptr || out == nullptr) return false;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  auto& state = EnsureWorkerEnvState(env);
  *out = state.config;
  return true;
}

bool UbiWorkerEnvIsMainThread(napi_env env) {
  UbiWorkerEnvConfig config;
  UbiWorkerEnvGetConfig(env, &config);
  return config.is_main_thread;
}

bool UbiWorkerEnvIsInternalThread(napi_env env) {
  UbiWorkerEnvConfig config;
  UbiWorkerEnvGetConfig(env, &config);
  return config.is_internal_thread;
}

bool UbiWorkerEnvOwnsProcessState(napi_env env) {
  UbiWorkerEnvConfig config;
  UbiWorkerEnvGetConfig(env, &config);
  return config.owns_process_state;
}

bool UbiWorkerEnvSharesEnvironment(napi_env env) {
  UbiWorkerEnvConfig config;
  UbiWorkerEnvGetConfig(env, &config);
  return config.share_env;
}

bool UbiWorkerEnvTracksUnmanagedFds(napi_env env) {
  UbiWorkerEnvConfig config;
  UbiWorkerEnvGetConfig(env, &config);
  return config.tracks_unmanaged_fds;
}

void UbiWorkerEnvAddUnmanagedFd(napi_env env, int fd) {
  if (env == nullptr || fd < 0) return;
  std::string warning;
  {
    std::lock_guard<std::mutex> lock(g_worker_env_mu);
    auto& state = EnsureWorkerEnvState(env);
    if (!state.config.tracks_unmanaged_fds) return;
    auto [_, inserted] = state.unmanaged_fds.emplace(fd);
    if (!inserted) {
      warning = "File descriptor " + std::to_string(fd) + " opened in unmanaged mode twice";
    }
  }
  if (!warning.empty()) EmitProcessWarning(env, warning);
}

void UbiWorkerEnvRemoveUnmanagedFd(napi_env env, int fd) {
  if (env == nullptr || fd < 0) return;
  std::string warning;
  {
    std::lock_guard<std::mutex> lock(g_worker_env_mu);
    auto& state = EnsureWorkerEnvState(env);
    if (!state.config.tracks_unmanaged_fds) return;
    const size_t removed = state.unmanaged_fds.erase(fd);
    if (removed == 0) {
      warning = "File descriptor " + std::to_string(fd) + " closed but not opened in unmanaged mode";
    }
  }
  if (!warning.empty()) EmitProcessWarning(env, warning);
}

bool UbiWorkerEnvStopRequested(napi_env env) {
  if (env == nullptr) return false;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  auto& state = EnsureWorkerEnvState(env);
  return state.stop_requested;
}

int32_t UbiWorkerEnvThreadId(napi_env env) {
  UbiWorkerEnvConfig config;
  UbiWorkerEnvGetConfig(env, &config);
  return config.thread_id;
}

std::string UbiWorkerEnvThreadName(napi_env env) {
  UbiWorkerEnvConfig config;
  UbiWorkerEnvGetConfig(env, &config);
  return config.thread_name;
}

std::array<double, 4> UbiWorkerEnvResourceLimits(napi_env env) {
  UbiWorkerEnvConfig config;
  UbiWorkerEnvGetConfig(env, &config);
  return config.resource_limits;
}

std::string UbiWorkerEnvGetProcessTitle(napi_env env) {
  UbiWorkerEnvConfig config;
  UbiWorkerEnvGetConfig(env, &config);
  return config.local_process_title;
}

void UbiWorkerEnvSetProcessTitle(napi_env env, const std::string& title) {
  if (env == nullptr) return;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  auto& state = EnsureWorkerEnvState(env);
  state.config.local_process_title = title;
}

uint32_t UbiWorkerEnvGetDebugPort(napi_env env) {
  UbiWorkerEnvConfig config;
  UbiWorkerEnvGetConfig(env, &config);
  return config.local_debug_port;
}

void UbiWorkerEnvSetDebugPort(napi_env env, uint32_t port) {
  if (env == nullptr) return;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  auto& state = EnsureWorkerEnvState(env);
  state.config.local_debug_port = port;
}

std::map<std::string, std::string> UbiWorkerEnvSnapshotEnvVars(napi_env env) {
  if (env == nullptr) return {};
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  auto& state = EnsureWorkerEnvState(env);
  return state.config.env_vars;
}

void UbiWorkerEnvSetLocalEnvVar(napi_env env, const std::string& key, const std::string& value) {
  if (env == nullptr || key.empty()) return;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  auto& state = EnsureWorkerEnvState(env);
  state.config.env_vars[key] = value;
}

void UbiWorkerEnvUnsetLocalEnvVar(napi_env env, const std::string& key) {
  if (env == nullptr || key.empty()) return;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  auto& state = EnsureWorkerEnvState(env);
  state.config.env_vars.erase(key);
}

void UbiWorkerEnvRequestStop(napi_env env) {
  if (env == nullptr) return;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  auto& state = EnsureWorkerEnvState(env);
  state.stop_requested = true;
}

void UbiWorkerEnvForget(napi_env env) {
  if (env == nullptr) return;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  g_worker_env_states.erase(env);
}

napi_value UbiWorkerEnvGetBinding(napi_env env) {
  if (env == nullptr) return nullptr;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  auto& state = EnsureWorkerEnvState(env);
  return GetRefValue(env, state.binding_ref);
}

void UbiWorkerEnvSetBinding(napi_env env, napi_value binding) {
  if (env == nullptr) return;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  auto& state = EnsureWorkerEnvState(env);
  SetRefValue(env, &state.binding_ref, binding);
}

napi_value UbiWorkerEnvGetEnvMessagePort(napi_env env) {
  if (env == nullptr) return nullptr;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  auto& state = EnsureWorkerEnvState(env);
  return GetRefValue(env, state.env_message_port_ref);
}

void UbiWorkerEnvSetEnvMessagePort(napi_env env, napi_value port) {
  if (env == nullptr) return;
  std::lock_guard<std::mutex> lock(g_worker_env_mu);
  auto& state = EnsureWorkerEnvState(env);
  SetRefValue(env, &state.env_message_port_ref, port);
}

internal_binding::UbiMessagePortDataPtr UbiWorkerEnvGetEnvMessagePortData(napi_env env) {
  UbiWorkerEnvConfig config;
  UbiWorkerEnvGetConfig(env, &config);
  return config.env_message_port_data;
}
