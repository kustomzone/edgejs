#ifndef UBI_INTERNAL_BINDING_ASYNC_WRAP_H_
#define UBI_INTERNAL_BINDING_ASYNC_WRAP_H_

#include "node_api.h"

namespace internal_binding {

napi_value AsyncWrapGetHooksObject(napi_env env);
napi_value AsyncWrapGetCallbackTrampoline(napi_env env);

}  // namespace internal_binding

#endif  // UBI_INTERNAL_BINDING_ASYNC_WRAP_H_
