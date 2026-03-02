#ifndef UNODE_TCP_WRAP_H_
#define UNODE_TCP_WRAP_H_

#include "node_api.h"
#include <uv.h>

void UnodeInstallTcpWrapBinding(napi_env env);
uv_stream_t* UnodeTcpWrapGetStream(napi_env env, napi_value value);

#endif  // UNODE_TCP_WRAP_H_
