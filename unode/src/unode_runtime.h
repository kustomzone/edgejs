#ifndef UNODE_RUNTIME_H_
#define UNODE_RUNTIME_H_

#include <string>

#include "js_native_api.h"

napi_status UnodeInstallConsole(napi_env env);
napi_status UnodeInstallUnofficialNapiTestingUntilGc(napi_env env, napi_value target);
int UnodeRunScriptSource(napi_env env, const char* source_text, std::string* error_out);
int UnodeRunScriptFile(napi_env env, const char* script_path, std::string* error_out);

#endif  // UNODE_RUNTIME_H_
