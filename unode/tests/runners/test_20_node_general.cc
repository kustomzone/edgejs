#include <string>

#include "test_env.h"
#include "upstream_js_test.h"

extern "C" napi_value napi_register_module_v1(napi_env env, napi_value exports);

class Test20NodeGeneral : public FixtureTestBase {};

TEST_F(Test20NodeGeneral, PortedCoreFlow) {
  EnvScope s(runtime_.get());
  napi_value exports = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
  ASSERT_NE(napi_register_module_v1(s.env, exports), nullptr);

  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "__ng", exports), napi_ok);

  auto run_js = [&](const char* source_text) {
    std::string wrapped = std::string("(() => { ") + source_text + " })();";
    return RunScript(s, wrapped, source_text);
  };

  ASSERT_TRUE(run_js(R"JS(
if (typeof __ng.filename !== 'string') throw new Error('filenameType');
if (!__ng.filename.startsWith('file://')) throw new Error('filenamePrefix');
const v = __ng.testGetNodeVersion();
if (!Array.isArray(v) || v.length !== 4) throw new Error('versionShape');
if (!Number.isInteger(v[0]) || !Number.isInteger(v[1]) || !Number.isInteger(v[2])) throw new Error('versionNums');
if (typeof v[3] !== 'string' || v[3].length === 0) throw new Error('versionRelease');
)JS"));
}
