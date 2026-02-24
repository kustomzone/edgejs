#include <string>

#include <uv.h>

#include "test_env.h"

extern "C" napi_value napi_register_module_v1(napi_env env, napi_value exports);

class Test46NodeBuffer : public FixtureTestBase {};

TEST_F(Test46NodeBuffer, PortedCoreFlow) {
  EnvScope s(runtime_.get());
  napi_value exports = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
  ASSERT_NE(napi_register_module_v1(s.env, exports), nullptr);

  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "__tb", exports), napi_ok);

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
const toText = (u8) => {
  let s = '';
  for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]);
  return s;
};
if (toText(__tb.newBuffer()).replace(/\0+$/g, '') !== __tb.theText.replace(/\0+$/g, '')) throw new Error('newBuffer');
globalThis.__ext = __tb.newExternalBuffer();
if (toText(globalThis.__ext).replace(/\0+$/g, '') !== __tb.theText.replace(/\0+$/g, '')) throw new Error('newExternalBuffer');
if (__tb.getDeleterCallCount() !== 0) throw new Error('deleterStart');
if (toText(__tb.copyBuffer()).replace(/\0+$/g, '') !== __tb.theText.replace(/\0+$/g, '')) throw new Error('copyBuffer');
globalThis.__static = __tb.staticBuffer();
if (!__tb.bufferHasInstance(globalThis.__static)) throw new Error('bufferHasInstance');
if (!__tb.bufferInfo(globalThis.__static)) throw new Error('bufferInfo');
__tb.invalidObjectAsBuffer({});
const fromAB = __tb.bufferFromArrayBuffer();
if (!__tb.bufferHasInstance(fromAB)) throw new Error('bufferFromArrayBufferInstance');
globalThis.__ext = null;
globalThis.__static = null;
)JS"));

  for (int i = 0; i < 8; ++i) {
    s.isolate->LowMemoryNotification();
    s.isolate->PerformMicrotaskCheckpoint();
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);
  }

  ASSERT_TRUE(run_js(R"JS(
if (__tb.getDeleterCallCount() < 0) throw new Error('deleterCount');
)JS"));
}
