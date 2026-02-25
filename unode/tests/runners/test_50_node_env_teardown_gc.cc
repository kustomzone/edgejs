#include <string>

#include "test_env.h"
#include "upstream_js_test.h"

extern "C" napi_value napi_register_module_v1(napi_env env, napi_value exports);

class Test50NodeEnvTeardownGc : public FixtureTestBase {};

TEST_F(Test50NodeEnvTeardownGc, PortedCoreFlow) {
  {
    EnvScope s(runtime_.get());
    napi_value exports = nullptr;
    ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
    ASSERT_NE(napi_register_module_v1(s.env, exports), nullptr);

    napi_value global = nullptr;
    ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
    ASSERT_EQ(napi_set_named_property(s.env, global, "__tetg", exports), napi_ok);

    auto run_js = [&](const char* source_text) {
      std::string wrapped = std::string("(() => { 'use strict'; ") + source_text + " })();";
      return RunScript(s, wrapped, source_text);
    };

    ASSERT_TRUE(run_js(R"JS(
globalThis.__cleanupCount = 0;
globalThis.cleanup = () => { globalThis.__cleanupCount++; };
globalThis.it = new __tetg.MyObject();
)JS"));

    s.isolate->LowMemoryNotification();
    s.isolate->PerformMicrotaskCheckpoint();
  }

  SUCCEED();
}
