#include <string>

#include "test_env.h"

extern "C" napi_value napi_register_module_v1(napi_env env, napi_value exports);

class Test54NodeMakeCallback : public FixtureTestBase {};

TEST_F(Test54NodeMakeCallback, PortedCoreFlow) {
  EnvScope s(runtime_.get());
  napi_value exports = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
  ASSERT_NE(napi_register_module_v1(s.env, exports), nullptr);

  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "__tmc", exports), napi_ok);

  auto run_js = [&](const char* source_text) {
    v8::TryCatch tc(s.isolate);
    std::string wrapped = std::string("(() => { 'use strict'; ") + source_text + " })();";
    v8::Local<v8::String> source =
        v8::String::NewFromUtf8(s.isolate, wrapped.c_str(), v8::NewStringType::kNormal)
            .ToLocalChecked();
    v8::Local<v8::Script> script;
    if (!v8::Script::Compile(s.context, source).ToLocal(&script)) return false;
    v8::Local<v8::Value> out;
    if (!script->Run(s.context).ToLocal(&out)) {
      if (tc.HasCaught()) {
        v8::String::Utf8Value msg(s.isolate, tc.Exception());
        ADD_FAILURE() << "JS exception: " << (*msg ? *msg : "<empty>")
                      << " while running: " << source_text;
      }
      return false;
    }
    return true;
  };

  ASSERT_TRUE(run_js(R"JS(
const resource = {};
const recv = { x: 1 };
const r1 = __tmc.makeCallback(resource, recv, function() {
  if (this !== recv) throw new Error('recv0');
  return 42;
});
if (r1 !== 42) throw new Error('r1');

const r2 = __tmc.makeCallback(resource, recv, function(a, b, c) {
  if (this !== recv) throw new Error('recv3');
  if (a !== 1 || b !== 2 || c !== 3) throw new Error('args3');
  return a + b + c;
}, 1, 2, 3);
if (r2 !== 6) throw new Error('r2');
)JS"));
}
