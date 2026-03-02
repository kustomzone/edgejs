#ifndef UNODE_STREAM_WRAP_H_
#define UNODE_STREAM_WRAP_H_

#include "node_api.h"

enum UnodeStreamStateIndex : int {
  kUnodeReadBytesOrError = 0,
  kUnodeArrayBufferOffset = 1,
  kUnodeBytesWritten = 2,
  kUnodeLastWriteWasAsync = 3,
  kUnodeStreamStateLength = 4,
};

void UnodeInstallStreamWrapBinding(napi_env env);
int32_t* UnodeGetStreamBaseState();

#endif  // UNODE_STREAM_WRAP_H_
