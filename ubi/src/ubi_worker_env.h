#ifndef UBI_WORKER_ENV_H_
#define UBI_WORKER_ENV_H_

#include <array>
#include <map>
#include <string>

#include "node_api.h"
#include "internal_binding/binding_messaging.h"

struct UbiWorkerEnvConfig {
  bool is_main_thread = true;
  bool is_internal_thread = false;
  bool owns_process_state = true;
  bool share_env = true;
  bool tracks_unmanaged_fds = false;
  int32_t thread_id = 0;
  std::string thread_name = "main";
  std::array<double, 4> resource_limits = {-1, -1, -1, -1};
  std::map<std::string, std::string> env_vars;
  internal_binding::UbiMessagePortDataPtr env_message_port_data;
  std::string local_process_title;
  uint32_t local_debug_port = 0;
};

void UbiWorkerEnvConfigure(napi_env env, const UbiWorkerEnvConfig& config);
bool UbiWorkerEnvGetConfig(napi_env env, UbiWorkerEnvConfig* out);

bool UbiWorkerEnvIsMainThread(napi_env env);
bool UbiWorkerEnvIsInternalThread(napi_env env);
bool UbiWorkerEnvOwnsProcessState(napi_env env);
bool UbiWorkerEnvSharesEnvironment(napi_env env);
bool UbiWorkerEnvTracksUnmanagedFds(napi_env env);
void UbiWorkerEnvAddUnmanagedFd(napi_env env, int fd);
void UbiWorkerEnvRemoveUnmanagedFd(napi_env env, int fd);
bool UbiWorkerEnvStopRequested(napi_env env);
int32_t UbiWorkerEnvThreadId(napi_env env);
std::string UbiWorkerEnvThreadName(napi_env env);
std::array<double, 4> UbiWorkerEnvResourceLimits(napi_env env);
std::string UbiWorkerEnvGetProcessTitle(napi_env env);
void UbiWorkerEnvSetProcessTitle(napi_env env, const std::string& title);
uint32_t UbiWorkerEnvGetDebugPort(napi_env env);
void UbiWorkerEnvSetDebugPort(napi_env env, uint32_t port);
std::map<std::string, std::string> UbiWorkerEnvSnapshotEnvVars(napi_env env);
void UbiWorkerEnvSetLocalEnvVar(napi_env env, const std::string& key, const std::string& value);
void UbiWorkerEnvUnsetLocalEnvVar(napi_env env, const std::string& key);
void UbiWorkerEnvRequestStop(napi_env env);
void UbiWorkerEnvForget(napi_env env);
void UbiWorkerEnvRunCleanup(napi_env env);

napi_value UbiWorkerEnvGetBinding(napi_env env);
void UbiWorkerEnvSetBinding(napi_env env, napi_value binding);

napi_value UbiWorkerEnvGetEnvMessagePort(napi_env env);
void UbiWorkerEnvSetEnvMessagePort(napi_env env, napi_value port);
internal_binding::UbiMessagePortDataPtr UbiWorkerEnvGetEnvMessagePortData(napi_env env);

#endif  // UBI_WORKER_ENV_H_
