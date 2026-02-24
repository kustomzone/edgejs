#ifndef NAPI_V8_UPSTREAM_JS_TEST_H_
#define NAPI_V8_UPSTREAM_JS_TEST_H_

#include <fstream>
#include <sstream>
#include <string>

#include "test_env.h"

inline std::string ReadTextFile(const std::string& path) {
  std::ifstream in(path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

inline bool RunScript(EnvScope& s, const std::string& source_text, const char* label) {
  v8::TryCatch tc(s.isolate);
  v8::Local<v8::String> source =
      v8::String::NewFromUtf8(s.isolate, source_text.c_str(), v8::NewStringType::kNormal)
          .ToLocalChecked();
  v8::Local<v8::Script> script;
  if (!v8::Script::Compile(s.context, source).ToLocal(&script)) return false;
  v8::Local<v8::Value> out;
  if (!script->Run(s.context).ToLocal(&out)) {
    if (tc.HasCaught()) {
      v8::String::Utf8Value msg(s.isolate, tc.Exception());
      ADD_FAILURE() << "JS exception (" << label << "): " << (*msg ? *msg : "<empty>");
    }
    return false;
  }
  return true;
}

inline bool InstallUpstreamJsShim(EnvScope& s, napi_value addon_exports) {
  napi_value global = nullptr;
  if (napi_get_global(s.env, &global) != napi_ok) return false;
  if (napi_set_named_property(s.env, global, "__napi_test_addon", addon_exports) != napi_ok) {
    return false;
  }

  const char* shim = R"JS(
(() => {
  'use strict';
  const __mustCallRecords = [];
  globalThis.common = {
    buildType: 'Debug',
    mustCall(fn, expected = 1) {
      if (typeof fn !== 'function') throw new Error('mustCall fn');
      const rec = { called: 0, expected };
      __mustCallRecords.push(rec);
      return function(...args) {
        rec.called++;
        return fn.apply(this, args);
      };
    },
    mustNotCall(message) {
      return function() {
        throw new Error(message || 'mustNotCall');
      };
    }
  };

  globalThis.assert = {
    strictEqual(actual, expected, message) {
      if (actual !== expected) throw new Error(message || `strictEqual: ${actual} !== ${expected}`);
    },
    notStrictEqual(actual, expected, message) {
      if (actual === expected) throw new Error(message || `notStrictEqual: ${actual} === ${expected}`);
    },
    ok(value, message) {
      if (!value) throw new Error(message || 'assert.ok failed');
    }
  };

  globalThis.require = function(spec) {
    if (spec === '../../common') return globalThis.common;
    if (spec === 'assert') return globalThis.assert;
    if (spec.startsWith('./build/')) return globalThis.__napi_test_addon;
    throw new Error(`Unsupported require: ${spec}`);
  };

  globalThis.__napi_verify_must_call = function() {
    for (const rec of __mustCallRecords) {
      if (rec.called !== rec.expected) {
        throw new Error(`mustCall mismatch: called=${rec.called}, expected=${rec.expected}`);
      }
    }
  };
})();
)JS";

  return RunScript(s, shim, "shim");
}

inline bool RunUpstreamJsFile(EnvScope& s, const std::string& path) {
  const std::string source = ReadTextFile(path);
  if (source.empty()) {
    ADD_FAILURE() << "Unable to read upstream JS file: " << path;
    return false;
  }
  if (!RunScript(s, source, path.c_str())) return false;
  return RunScript(s, "__napi_verify_must_call();", "must-call-verification");
}

#endif  // NAPI_V8_UPSTREAM_JS_TEST_H_
