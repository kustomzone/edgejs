#include "internal_binding/dispatch.h"

#include <array>
#include <string_view>

#include "internal_binding/helpers.h"

namespace internal_binding {

napi_value ResolveAsyncWrap(napi_env env, const ResolveOptions& options);
napi_value ResolveBlob(napi_env env, const ResolveOptions& options);
napi_value ResolveBuffer(napi_env env, const ResolveOptions& options);
napi_value ResolveBuiltins(napi_env env, const ResolveOptions& options);
napi_value ResolveCaresWrap(napi_env env, const ResolveOptions& options);
napi_value ResolveConfig(napi_env env, const ResolveOptions& options);
napi_value ResolveConstants(napi_env env, const ResolveOptions& options);
napi_value ResolveContextify(napi_env env, const ResolveOptions& options);
napi_value ResolveCredentials(napi_env env, const ResolveOptions& options);
napi_value ResolveCrypto(napi_env env, const ResolveOptions& options);
napi_value ResolveEncodingBinding(napi_env env, const ResolveOptions& options);
napi_value ResolveErrors(napi_env env, const ResolveOptions& options);
napi_value ResolveFs(napi_env env, const ResolveOptions& options);
napi_value ResolveHttpParser(napi_env env, const ResolveOptions& options);
napi_value ResolveModuleWrap(napi_env env, const ResolveOptions& options);
napi_value ResolveModules(napi_env env, const ResolveOptions& options);
napi_value ResolveMksnapshot(napi_env env, const ResolveOptions& options);
napi_value ResolveMessaging(napi_env env, const ResolveOptions& options);
napi_value ResolveOptionsBinding(napi_env env, const ResolveOptions& options);
napi_value ResolveOs(napi_env env, const ResolveOptions& options);
napi_value ResolvePerformance(napi_env env, const ResolveOptions& options);
napi_value ResolvePermission(napi_env env, const ResolveOptions& options);
napi_value ResolvePipeWrap(napi_env env, const ResolveOptions& options);
napi_value ResolveProcessMethods(napi_env env, const ResolveOptions& options);
napi_value ResolveProcessWrap(napi_env env, const ResolveOptions& options);
napi_value ResolveReport(napi_env env, const ResolveOptions& options);
napi_value ResolveSignalWrap(napi_env env, const ResolveOptions& options);
napi_value ResolveSpawnSync(napi_env env, const ResolveOptions& options);
napi_value ResolveStreamWrap(napi_env env, const ResolveOptions& options);
napi_value ResolveStringDecoder(napi_env env, const ResolveOptions& options);
napi_value ResolveSymbols(napi_env env, const ResolveOptions& options);
napi_value ResolveTaskQueue(napi_env env, const ResolveOptions& options);
napi_value ResolveTcpWrap(napi_env env, const ResolveOptions& options);
napi_value ResolveTimers(napi_env env, const ResolveOptions& options);
napi_value ResolveTraceEvents(napi_env env, const ResolveOptions& options);
napi_value ResolveTtyWrap(napi_env env, const ResolveOptions& options);
napi_value ResolveTypes(napi_env env, const ResolveOptions& options);
napi_value ResolveUdpWrap(napi_env env, const ResolveOptions& options);
napi_value ResolveUrl(napi_env env, const ResolveOptions& options);
napi_value ResolveUrlPattern(napi_env env, const ResolveOptions& options);
napi_value ResolveUtil(napi_env env, const ResolveOptions& options);
napi_value ResolveUv(napi_env env, const ResolveOptions& options);
napi_value ResolveWasmWebApi(napi_env env, const ResolveOptions& options);
napi_value ResolveWorker(napi_env env, const ResolveOptions& options);

namespace {

using ResolverFn = napi_value (*)(napi_env env, const ResolveOptions& options);

struct BindingResolverEntry {
  std::string_view name;
  ResolverFn resolver;
};

constexpr std::array<BindingResolverEntry, 44> kResolvers = {{
    {"async_wrap", ResolveAsyncWrap},
    {"blob", ResolveBlob},
    {"buffer", ResolveBuffer},
    {"builtins", ResolveBuiltins},
    {"cares_wrap", ResolveCaresWrap},
    {"config", ResolveConfig},
    {"constants", ResolveConstants},
    {"contextify", ResolveContextify},
    {"credentials", ResolveCredentials},
    {"crypto", ResolveCrypto},
    {"encoding_binding", ResolveEncodingBinding},
    {"errors", ResolveErrors},
    {"fs", ResolveFs},
    {"http_parser", ResolveHttpParser},
    {"module_wrap", ResolveModuleWrap},
    {"modules", ResolveModules},
    {"mksnapshot", ResolveMksnapshot},
    {"messaging", ResolveMessaging},
    {"options", ResolveOptionsBinding},
    {"os", ResolveOs},
    {"performance", ResolvePerformance},
    {"permission", ResolvePermission},
    {"pipe_wrap", ResolvePipeWrap},
    {"process_methods", ResolveProcessMethods},
    {"process_wrap", ResolveProcessWrap},
    {"report", ResolveReport},
    {"signal_wrap", ResolveSignalWrap},
    {"spawn_sync", ResolveSpawnSync},
    {"stream_wrap", ResolveStreamWrap},
    {"string_decoder", ResolveStringDecoder},
    {"symbols", ResolveSymbols},
    {"task_queue", ResolveTaskQueue},
    {"tcp_wrap", ResolveTcpWrap},
    {"timers", ResolveTimers},
    {"trace_events", ResolveTraceEvents},
    {"tty_wrap", ResolveTtyWrap},
    {"types", ResolveTypes},
    {"udp_wrap", ResolveUdpWrap},
    {"url", ResolveUrl},
    {"url_pattern", ResolveUrlPattern},
    {"util", ResolveUtil},
    {"uv", ResolveUv},
    {"wasm_web_api", ResolveWasmWebApi},
    {"worker", ResolveWorker},
}};

}  // namespace

napi_value Resolve(napi_env env, const std::string& name, const ResolveOptions& options) {
  for (const auto& entry : kResolvers) {
    if (entry.name == name) {
      return entry.resolver(env, options);
    }
  }
  return Undefined(env);
}

}  // namespace internal_binding
