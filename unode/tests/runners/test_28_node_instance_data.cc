#include <string>

#include <uv.h>

#include "test_env.h"
#include "upstream_js_test.h"

extern "C" napi_value napi_register_module_v1(napi_env env, napi_value exports);

class Test28NodeInstanceData : public FixtureTestBase {};

TEST_F(Test28NodeInstanceData, PortedCoreFlow) {
  EnvScope s(runtime_.get());
  napi_value exports = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
  ASSERT_NE(napi_register_module_v1(s.env, exports), nullptr);

  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "__tid", exports), napi_ok);

  auto run_js = [&](const char* source_text) {
    std::string wrapped = std::string("(() => { ") + source_text + " })();";
    return RunScript(s, wrapped, source_text);
  };

  ASSERT_TRUE(run_js(R"JS(
globalThis.__asyncCbCount = 0;
globalThis.__tsfnCbCount = 0;
globalThis.__tsfnFinalizeCount = 0;
__tid.asyncWorkCallback(() => { globalThis.__asyncCbCount++; });
)JS"));

  uv_run(uv_default_loop(), UV_RUN_DEFAULT);
  s.isolate->PerformMicrotaskCheckpoint();

  ASSERT_TRUE(run_js(R"JS(
if (globalThis.__asyncCbCount !== 1) throw new Error('asyncWorkCallback');
__tid.testThreadsafeFunction(
  () => { globalThis.__tsfnCbCount++; },
  () => { globalThis.__tsfnFinalizeCount++; }
);
)JS"));
}
