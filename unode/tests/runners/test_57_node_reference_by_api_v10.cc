#include <string>

#include "test_env.h"

extern "C" napi_value napi_register_module_v1(napi_env env, napi_value exports);

class Test57NodeReferenceByApiV10 : public FixtureTestBase {};

TEST_F(Test57NodeReferenceByApiV10, PortedCoreFlow) {
  EnvScope s(runtime_.get());
  napi_value exports = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
  ASSERT_NE(napi_register_module_v1(s.env, exports), nullptr);

  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "__trv10", exports), napi_ok);

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
const idxNum = __trv10.createRef(42);
if (__trv10.ref(idxNum) !== 2) throw new Error('refNum');
if (__trv10.unref(idxNum) !== 1) throw new Error('unrefNum1');
if (__trv10.unref(idxNum) !== 0) throw new Error('unrefNum0');
if (__trv10.getRefValue(idxNum) !== 42) throw new Error('numValue');
__trv10.deleteRef(idxNum);

const idxObj = __trv10.createRef({ p: 1 });
if (__trv10.getRefValue(idxObj) === undefined) throw new Error('objValue');
__trv10.deleteRef(idxObj);
)JS"));
}
