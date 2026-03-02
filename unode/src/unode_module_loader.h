#ifndef UNODE_MODULE_LOADER_H_
#define UNODE_MODULE_LOADER_H_

#include "node_api.h"

napi_status UnodeInstallModuleLoader(napi_env env, const char* entry_script_path);

// Store primordials and internalBinding in loader state so they are passed from C++ when calling
// the module wrapper (Node-aligned: fn->Call(context, undefined, argc, argv) with argv from C++).
// Call after the bootstrap prelude so every user module receives the same reference.
void UnodeSetPrimordials(napi_env env, napi_value primordials);
void UnodeSetInternalBinding(napi_env env, napi_value internal_binding);

#endif  // UNODE_MODULE_LOADER_H_
