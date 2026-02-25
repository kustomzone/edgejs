#include <string>

#include "test_env.h"
#include "upstream_js_test.h"

extern "C" napi_value napi_register_module_v1(napi_env env, napi_value exports);

class Test62NodeWorkerBufferCallback : public FixtureTestBase {};

TEST_F(Test62NodeWorkerBufferCallback, PortedCoreFlow) {
  EnvScope s(runtime_.get());
  napi_value exports = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
  napi_value module_value = napi_register_module_v1(s.env, exports);
  ASSERT_NE(module_value, nullptr);

  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "__twbc", module_value), napi_ok);

  auto run_js = [&](const char* source_text) {
    std::string wrapped = std::string("(() => { 'use strict'; ") + source_text + " })();";
    return RunScript(s, wrapped, source_text);
  };

  ASSERT_TRUE(run_js(R"JS(
if (!__twbc || typeof __twbc.getFreeCallCount !== 'function') throw new Error('getFreeCallCount');
if (!__twbc.buffer) throw new Error('bufferExport');
const n = __twbc.getFreeCallCount();
if (typeof n !== 'number' || n < 0) throw new Error('countType');
)JS"));
}
