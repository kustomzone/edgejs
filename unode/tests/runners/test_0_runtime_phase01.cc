#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

#include "test_env.h"
#include "unode_runtime.h"

class Test0RuntimePhase01 : public FixtureTestBase {};

namespace {

std::string g_last_console_log;

std::string ValueToUtf8(napi_env env, napi_value value) {
  napi_value string_value = nullptr;
  if (napi_coerce_to_string(env, value, &string_value) != napi_ok || string_value == nullptr) {
    return "";
  }
  size_t length = 0;
  if (napi_get_value_string_utf8(env, string_value, nullptr, 0, &length) != napi_ok) {
    return "";
  }
  std::string out(length + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, string_value, out.data(), out.size(), &copied) != napi_ok) {
    return "";
  }
  out.resize(copied);
  return out;
}

napi_value ConsoleLogCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) == napi_ok && argc > 0) {
    g_last_console_log = ValueToUtf8(env, args[0]);
  } else {
    g_last_console_log.clear();
  }
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

void InstallConsoleLog(napi_env env) {
  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(env, &global), napi_ok);

  napi_value console_obj = nullptr;
  ASSERT_EQ(napi_create_object(env, &console_obj), napi_ok);
  napi_value log_fn = nullptr;
  ASSERT_EQ(napi_create_function(env, "log", NAPI_AUTO_LENGTH, ConsoleLogCallback, nullptr, &log_fn), napi_ok);
  ASSERT_EQ(napi_set_named_property(env, console_obj, "log", log_fn), napi_ok);
  ASSERT_EQ(napi_set_named_property(env, global, "console", console_obj), napi_ok);
}

std::string WriteTempScript(const std::string& stem, const std::string& contents) {
  const auto temp_dir = std::filesystem::temp_directory_path();
  const auto unique_name =
      stem + "_" + std::to_string(static_cast<unsigned long long>(std::hash<std::string>{}(contents))) + ".js";
  const auto script_path = temp_dir / unique_name;
  std::ofstream out(script_path);
  out << contents;
  out.close();
  return script_path.string();
}

void RemoveTempScript(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

}  // namespace

TEST_F(Test0RuntimePhase01, ValidFixtureScriptReturnsZero) {
  EnvScope s(runtime_.get());
  const std::string script_path = std::string(NAPI_V8_ROOT_PATH) + "/tests/fixtures/phase0_hello.js";
  ASSERT_TRUE(std::filesystem::exists(script_path)) << "Missing fixture: " << script_path;
  g_last_console_log.clear();
  InstallConsoleLog(s.env);

  std::string error;
  const int exit_code = UnodeRunScriptFile(s.env, script_path.c_str(), &error);
  EXPECT_EQ(exit_code, 0) << "error=" << error << ", fixture=" << script_path;
  EXPECT_TRUE(error.empty()) << "error=" << error;
  EXPECT_EQ(g_last_console_log, "hello from unode");
}

TEST_F(Test0RuntimePhase01, ThrownErrorReturnsNonZero) {
  EnvScope s(runtime_.get());
  const std::string script_path = WriteTempScript("unode_phase01_throw", "throw new Error('boom from unode');");

  std::string error;
  const int exit_code = UnodeRunScriptFile(s.env, script_path.c_str(), &error);
  EXPECT_EQ(exit_code, 1);
  EXPECT_NE(error.find("boom from unode"), std::string::npos);

  RemoveTempScript(script_path);
}

TEST_F(Test0RuntimePhase01, SyntaxErrorReturnsNonZero) {
  EnvScope s(runtime_.get());
  const std::string script_path = WriteTempScript("unode_phase01_syntax", "function (");

  std::string error;
  const int exit_code = UnodeRunScriptFile(s.env, script_path.c_str(), &error);
  EXPECT_EQ(exit_code, 1);
  EXPECT_FALSE(error.empty());

  RemoveTempScript(script_path);
}

TEST_F(Test0RuntimePhase01, SourcePathCanBeTestedIndependently) {
  EnvScope s(runtime_.get());

  std::string error;
  const int exit_code = UnodeRunScriptSource(s.env, "globalThis.__phase01_source = 'ok';", &error);
  EXPECT_EQ(exit_code, 0);
  EXPECT_TRUE(error.empty());

  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  napi_value value = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, global, "__phase01_source", &value), napi_ok);
  EXPECT_EQ(ValueToUtf8(s.env, value), "ok");
}

TEST_F(Test0RuntimePhase01, EmptySourceReturnsNonZero) {
  EnvScope s(runtime_.get());

  std::string error;
  const int exit_code = UnodeRunScriptSource(s.env, "", &error);
  EXPECT_EQ(exit_code, 1);
  EXPECT_EQ(error, "Empty script source");
}
