#ifndef UBI_ERRORS_BINDING_H_
#define UBI_ERRORS_BINDING_H_

#include <string>

#include "node_api.h"

napi_value UbiGetOrCreateErrorsBinding(napi_env env);
std::string UbiFormatFatalExceptionAfterInspector(napi_env env, napi_value exception);

#endif  // UBI_ERRORS_BINDING_H_
