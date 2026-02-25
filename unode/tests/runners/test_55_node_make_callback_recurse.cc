#include <string>

#include "test_env.h"
#include "upstream_js_test.h"

extern "C" napi_value napi_register_module_v1(napi_env env, napi_value exports);

class Test55NodeMakeCallbackRecurse : public FixtureTestBase {};

TEST_F(Test55NodeMakeCallbackRecurse, PortedCoreFlow) {
  EnvScope s(runtime_.get());
  napi_value exports = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
  ASSERT_NE(napi_register_module_v1(s.env, exports), nullptr);

  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "__tmcr", exports), napi_ok);

  auto run_js = [&](const char* source_text) {
    std::string wrapped = std::string("(() => { 'use strict'; ") + source_text + " })();";
    return RunScript(s, wrapped, source_text);
  };

  ASSERT_TRUE(run_js(R"JS(
let caught = '';
try {
  __tmcr.makeCallback({}, function() {
    throw new Error('hi from domain error');
  });
} catch (e) {
  caught = e.message;
}
if (caught !== 'hi from domain error') throw new Error('recurseThrow');

const recv = {};
__tmcr.makeCallback(recv, function() {
  __tmcr.makeCallback(recv, function() {});
});
)JS"));
}
