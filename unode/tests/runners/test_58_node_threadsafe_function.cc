#include <string>

#include "test_env.h"
#include "upstream_js_test.h"

extern "C" napi_value napi_register_module_v1(napi_env env, napi_value exports);

class Test58NodeThreadsafeFunction : public FixtureTestBase {};

TEST_F(Test58NodeThreadsafeFunction, PortedCoreFlow) {
  EnvScope s(runtime_.get());
  napi_value exports = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
  ASSERT_NE(napi_register_module_v1(s.env, exports), nullptr);

  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "__ttsf", exports), napi_ok);

  auto run_js = [&](const char* source_text) {
    std::string wrapped = std::string("(() => { 'use strict'; ") + source_text + " })();";
    return RunScript(s, wrapped, source_text);
  };

  ASSERT_TRUE(run_js(R"JS(
if (typeof __ttsf.ARRAY_LENGTH !== 'number') throw new Error('arrayLength');
if (typeof __ttsf.MAX_QUEUE_SIZE !== 'number') throw new Error('maxQueueSize');
let finalizeCalled = false;
__ttsf.CallIntoModule(
  () => {},
  {},
  'tsfn',
  () => { finalizeCalled = true; }
);
)JS"));
}
