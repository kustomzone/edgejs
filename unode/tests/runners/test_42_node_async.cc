#include <string>

#include <uv.h>

#include "test_env.h"
#include "upstream_js_test.h"

extern "C" napi_value napi_register_module_v1(napi_env env, napi_value exports);

class Test42NodeAsync : public FixtureTestBase {};

TEST_F(Test42NodeAsync, PortedCoreFlow) {
  EnvScope s(runtime_.get());
  napi_value exports = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
  ASSERT_NE(napi_register_module_v1(s.env, exports), nullptr);

  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "__tna", exports), napi_ok);

  auto run_js = [&](const char* source_text) {
    std::string wrapped = std::string("(() => { 'use strict'; ") + source_text + " })();";
    return RunScript(s, wrapped, source_text);
  };

  ASSERT_TRUE(run_js(R"JS(
globalThis.__asyncState = {
  testOk: false,
  cancelOk: false,
  repeatedOk: false,
};
__tna.Test(5, {}, (err, val) => {
  globalThis.__asyncState.testOk = (err === null && val === 10);
});
__tna.TestCancel(() => {
  globalThis.__asyncState.cancelOk = true;
});
__tna.DoRepeatedWork((status) => {
  globalThis.__asyncState.repeatedOk = (status === 0);
});
)JS"));

  uv_run(uv_default_loop(), UV_RUN_DEFAULT);
  s.isolate->PerformMicrotaskCheckpoint();

  ASSERT_TRUE(run_js(R"JS(
if (!globalThis.__asyncState.testOk) throw new Error('testAsync');
if (!globalThis.__asyncState.cancelOk) throw new Error('cancelAsync');
if (!globalThis.__asyncState.repeatedOk) throw new Error('repeatedAsync');
)JS"));
}
