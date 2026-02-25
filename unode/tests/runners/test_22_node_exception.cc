#include <string>

#include "test_env.h"
#include "upstream_js_test.h"

extern "C" napi_value napi_register_module_v1(napi_env env, napi_value exports);

class Test22NodeException : public FixtureTestBase {};

TEST_F(Test22NodeException, PortedCoreFlow) {
  EnvScope s(runtime_.get());
  napi_value exports = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
  ASSERT_NE(napi_register_module_v1(s.env, exports), nullptr);

  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "__nex", exports), napi_ok);

  auto run_js = [&](const char* source_text) {
    std::string wrapped = std::string("(() => { ") + source_text + " })();";
    return RunScript(s, wrapped, source_text);
  };

  ASSERT_TRUE(run_js(R"JS(
const b = __nex.createExternalBuffer();
if (typeof b !== 'object') throw new Error('bufferObject');
)JS"));
}
