#include <string>

#include "test_env.h"

extern "C" napi_value napi_register_module_v1(napi_env env, napi_value exports);

class Test56NodeReferenceByApiV9 : public FixtureTestBase {};

TEST_F(Test56NodeReferenceByApiV9, PortedCoreFlow) {
  EnvScope s(runtime_.get());
  napi_value exports = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
  ASSERT_NE(napi_register_module_v1(s.env, exports), nullptr);

  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "__trv", exports), napi_ok);

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
const idxObj = __trv.createRef({ x: 1 });
if (__trv.ref(idxObj) !== 2) throw new Error('refObj');
if (__trv.unref(idxObj) !== 1) throw new Error('unrefObj1');
if (__trv.unref(idxObj) !== 0) throw new Error('unrefObj0');
if (__trv.getRefValue(idxObj) === undefined) throw new Error('getRefObj');
__trv.deleteRef(idxObj);

__trv.initFinalizeCount();
__trv.addFinalizer({});
if (__trv.getFinalizeCount() < 0) throw new Error('finalizerCount');
)JS"));
}
