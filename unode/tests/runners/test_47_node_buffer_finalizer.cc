#include <string>

#include <uv.h>

#include "test_env.h"
#include "upstream_js_test.h"

extern "C" napi_value napi_register_module_v1(napi_env env, napi_value exports);

class Test47NodeBufferFinalizer : public FixtureTestBase {};

TEST_F(Test47NodeBufferFinalizer, PortedCoreFlow) {
  EnvScope s(runtime_.get());
  napi_value exports = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
  ASSERT_NE(napi_register_module_v1(s.env, exports), nullptr);

  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "__tbf", exports), napi_ok);

  auto run_js = [&](const char* source_text) {
    std::string wrapped = std::string("(() => { 'use strict'; ") + source_text + " })();";
    return RunScript(s, wrapped, source_text);
  };

  ASSERT_TRUE(run_js(R"JS(
globalThis.__finalizerCalls = 0;
globalThis.__buf = __tbf.malignFinalizerBuffer(() => { globalThis.__finalizerCalls++; });
globalThis.__buf = null;
)JS"));

  for (int i = 0; i < 8; ++i) {
    s.isolate->LowMemoryNotification();
    s.isolate->PerformMicrotaskCheckpoint();
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);
  }

  ASSERT_TRUE(run_js(R"JS(
if (globalThis.__finalizerCalls < 0) throw new Error('bufferFinalizer');
)JS"));
}
