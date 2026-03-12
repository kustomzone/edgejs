#ifndef EDGE_ENVIRONMENT_RUNTIME_H_
#define EDGE_ENVIRONMENT_RUNTIME_H_

#include "edge_environment.h"

bool EdgeAttachEnvironmentForRuntime(napi_env env,
                                     const EdgeEnvironmentConfig* config = nullptr);

#endif  // EDGE_ENVIRONMENT_RUNTIME_H_
