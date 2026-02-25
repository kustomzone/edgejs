#include <string>

#include "test_env.h"
#include "upstream_js_test.h"

extern "C" napi_value napi_register_module_v1(napi_env env, napi_value exports);

class Test45NodeAsyncContext : public FixtureTestBase {};

TEST_F(Test45NodeAsyncContext, PortedCoreFlow) {
  EnvScope s(runtime_.get());
  napi_value exports = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
  ASSERT_NE(napi_register_module_v1(s.env, exports), nullptr);

  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "__tac", exports), napi_ok);

  auto run_js = [&](const char* source_text) {
    std::string wrapped = std::string("(() => { 'use strict'; ") + source_text + " })();";
    return RunScript(s, wrapped, source_text);
  };

  ASSERT_TRUE(run_js(R"JS(
const resourceWrap = __tac.createAsyncResource(undefined);
const recv = { marker: 42 };
const result = __tac.makeCallback(
  resourceWrap,
  recv,
  function(a, b) {
    if (this !== recv) throw new Error('recv');
    return a + b;
  },
  5,
  7
);
if (result !== 12) throw new Error('result');
__tac.destroyAsyncResource(resourceWrap);
)JS"));
}
