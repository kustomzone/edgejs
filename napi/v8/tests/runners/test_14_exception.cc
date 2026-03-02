#include "test_env.h"
#include "upstream_js_test.h"

extern "C" napi_value Init(napi_env env, napi_value exports);

class Test14Exception : public FixtureTestBase {};

TEST_F(Test14Exception, PortedCoreFlow) {
  EnvScope s(runtime_.get());
  napi_value exports = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
  (void)Init(s.env, exports);

  bool pending = false;
  ASSERT_EQ(napi_is_exception_pending(s.env, &pending), napi_ok);
  ASSERT_TRUE(pending);
  napi_value init_error = nullptr;
  ASSERT_EQ(napi_get_and_clear_last_exception(s.env, &init_error), napi_ok);

  napi_value binding = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, init_error, "binding", &binding), napi_ok);
  ASSERT_TRUE(InstallUpstreamJsShim(s, binding));
  ASSERT_TRUE(SetUpstreamRequireException(s, init_error));
  ASSERT_TRUE(RunUpstreamJsFile(
      s, std::string(NAPI_TESTS_ROOT_PATH) + "/js-native-api/test_exception/test.js"));
}
