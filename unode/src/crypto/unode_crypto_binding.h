#ifndef UNODE_CRYPTO_BINDING_H_
#define UNODE_CRYPTO_BINDING_H_

#include "node_api.h"

namespace unode::crypto {

void InstallCryptoBinding(napi_env env);

}  // namespace unode::crypto

#endif  // UNODE_CRYPTO_BINDING_H_
