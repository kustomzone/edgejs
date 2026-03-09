#ifndef UBI_RUNTIME_H_
#define UBI_RUNTIME_H_

#include <string>
#include <vector>

#include "node_api.h"

napi_status UbiInstallConsole(napi_env env);
napi_status UbiInstallProcessObject(napi_env env,
                                      const std::string& current_script_path,
                                      const std::vector<std::string>& exec_argv,
                                      const std::vector<std::string>& script_argv,
                                      const std::string& process_title);
napi_status UbiInstallUnofficialNapiTestingUntilGc(napi_env env, napi_value target);
int UbiRunScriptSource(napi_env env, const char* source_text, std::string* error_out);
int UbiRunScriptSourceWithLoop(napi_env env,
                               const char* source_text,
                               std::string* error_out,
                               bool keep_event_loop_alive);
int UbiRunScriptFile(napi_env env, const char* script_path, std::string* error_out);
int UbiRunScriptFileWithLoop(napi_env env,
                               const char* script_path,
                               std::string* error_out,
                               bool keep_event_loop_alive);
int UbiRunWorkerThreadMain(napi_env env,
                           const std::vector<std::string>& exec_argv,
                           std::string* error_out);
bool UbiInitializeOpenSslForCli(std::string* error_out);
void UbiSetScriptArgv(const std::vector<std::string>& script_argv);
void UbiSetExecArgv(const std::vector<std::string>& exec_argv);
bool UbiExecArgvHasFlag(const char* flag);

enum UbiMakeCallbackFlags : int {
  kUbiMakeCallbackNone = 0,
  // Mirrors Node's InternalCallbackScope::kSkipTaskQueues for critical paths
  // like HTTP parser callbacks that must not re-enter JS tick processing.
  kUbiMakeCallbackSkipTaskQueues = 1 << 0,
};

napi_status UbiMakeCallbackWithFlags(napi_env env,
                                     napi_value recv,
                                     napi_value callback,
                                     size_t argc,
                                     napi_value* argv,
                                     napi_value* result,
                                     int flags);
napi_status UbiCallCallbackWithDomain(napi_env env,
                                      napi_value recv,
                                      napi_value callback,
                                      size_t argc,
                                      napi_value* argv,
                                      napi_value* result);

napi_status UbiMakeCallback(napi_env env,
                              napi_value recv,
                              napi_value callback,
                              size_t argc,
                              napi_value* argv,
                              napi_value* result);
// Mirrors the top-level task-queue checkpoint that Node runs when unwinding an
// InternalCallbackScope. Use this after settling native promises from libuv
// callbacks that did not enter JS through UbiMakeCallback().
napi_status UbiRunCallbackScopeCheckpoint(napi_env env);
bool UbiHandlePendingExceptionNow(napi_env env, bool* handled_out);

#endif  // UBI_RUNTIME_H_
