#ifndef UNODE_PIPE_WRAP_H_
#define UNODE_PIPE_WRAP_H_

#include <uv.h>

#include "node_api.h"

void UnodeInstallPipeWrapBinding(napi_env env);
uv_stream_t* UnodePipeWrapGetStream(napi_env env, napi_value value);

#endif  // UNODE_PIPE_WRAP_H_
