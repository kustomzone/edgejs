#include <string>

#include <uv.h>

#include "test_env.h"
#include "upstream_js_test.h"

extern "C" napi_value napi_register_module_v1(napi_env env, napi_value exports);

class Test61NodeUvThreadpoolSize : public FixtureTestBase {};

TEST_F(Test61NodeUvThreadpoolSize, PortedCoreFlow) {
  EnvScope s(runtime_.get());
  napi_value exports = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
  napi_value module_value = napi_register_module_v1(s.env, exports);
  ASSERT_NE(module_value, nullptr);

  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "__tuvtp", module_value), napi_ok);

  auto run_js = [&](const char* source_text) {
    std::string wrapped = std::string("(() => { 'use strict'; ") + source_text + " })();";
    return RunScript(s, wrapped, source_text);
  };

  ASSERT_TRUE(run_js(R"JS(
if (typeof __tuvtp.test !== 'function') throw new Error('testExport');
)JS"));

  uv_run(uv_default_loop(), UV_RUN_DEFAULT);
  s.isolate->PerformMicrotaskCheckpoint();
}
