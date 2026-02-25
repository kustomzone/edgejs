#include <string>

#include "test_env.h"
#include "upstream_js_test.h"

extern "C" napi_value napi_register_module_v1(napi_env env, napi_value exports);

class Test43NodeHelloWorld : public FixtureTestBase {};

TEST_F(Test43NodeHelloWorld, PortedCoreFlow) {
  EnvScope s(runtime_.get());

  napi_value exports1 = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &exports1), napi_ok);
  ASSERT_NE(napi_register_module_v1(s.env, exports1), nullptr);

  napi_value exports2 = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &exports2), napi_ok);
  ASSERT_NE(napi_register_module_v1(s.env, exports2), nullptr);

  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "__h1", exports1), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "__h2", exports2), napi_ok);

  auto run_js = [&](const char* source_text) {
    std::string wrapped = std::string("(() => { 'use strict'; ") + source_text + " })();";
    return RunScript(s, wrapped, source_text);
  };

  ASSERT_TRUE(run_js(R"JS(
if (__h1.hello() !== 'world') throw new Error('hello1');
if (__h2.hello() !== 'world') throw new Error('hello2');
if (__h1.hello === __h2.hello) throw new Error('distinctFunctions');
)JS"));
}
