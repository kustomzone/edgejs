#ifndef UBI_WORKER_CONTROL_H_
#define UBI_WORKER_CONTROL_H_

#include <string>
#include <vector>

#include "node_api.h"

struct UbiWorkerReportEntry {
  int32_t thread_id = 0;
  std::string thread_name;
  std::string json;
};

void UbiWorkerStopAllForEnv(napi_env env);
std::vector<UbiWorkerReportEntry> UbiWorkerCollectReports(napi_env env);

#endif  // UBI_WORKER_CONTROL_H_
