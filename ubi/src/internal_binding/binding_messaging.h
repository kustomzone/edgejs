#ifndef UBI_INTERNAL_BINDING_BINDING_MESSAGING_H_
#define UBI_INTERNAL_BINDING_BINDING_MESSAGING_H_

#include <memory>

#include "node_api.h"

namespace internal_binding {

struct UbiMessagePortData;
using UbiMessagePortDataPtr = std::shared_ptr<UbiMessagePortData>;

UbiMessagePortDataPtr UbiCreateMessagePortData();
void UbiEntangleMessagePortData(const UbiMessagePortDataPtr& first,
                                const UbiMessagePortDataPtr& second);
UbiMessagePortDataPtr UbiGetMessagePortData(napi_env env, napi_value value);
napi_value UbiCreateMessagePortForData(napi_env env, const UbiMessagePortDataPtr& data);

}  // namespace internal_binding

#endif  // UBI_INTERNAL_BINDING_BINDING_MESSAGING_H_
