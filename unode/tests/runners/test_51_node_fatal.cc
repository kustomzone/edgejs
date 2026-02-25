#include <string>

#include <gtest/gtest.h>

#include "test_env.h"

extern "C" napi_value napi_register_module_v1(napi_env env, napi_value exports);

class Test51NodeFatal : public FixtureTestBase {};

TEST_F(Test51NodeFatal, FatalMessageDeath) {
  ASSERT_DEATH(
      {
        EnvScope s(runtime_.get());
        napi_value exports = nullptr;
        ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
        ASSERT_NE(napi_register_module_v1(s.env, exports), nullptr);
        napi_value global = nullptr;
        ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
        ASSERT_EQ(napi_set_named_property(s.env, global, "__tf", exports), napi_ok);

        napi_value script = nullptr;
        ASSERT_EQ(napi_create_string_utf8(s.env, "__tf.Test()", NAPI_AUTO_LENGTH, &script), napi_ok);
        napi_value result = nullptr;
        (void)napi_run_script(s.env, script, &result);
      },
      "FATAL ERROR: test_fatal::Test fatal message");
}

TEST_F(Test51NodeFatal, FatalStringLengthDeath) {
  ASSERT_DEATH(
      {
        EnvScope s(runtime_.get());
        napi_value exports = nullptr;
        ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
        ASSERT_NE(napi_register_module_v1(s.env, exports), nullptr);
        napi_value global = nullptr;
        ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
        ASSERT_EQ(napi_set_named_property(s.env, global, "__tf", exports), napi_ok);

        napi_value script = nullptr;
        ASSERT_EQ(napi_create_string_utf8(s.env, "__tf.TestStringLength()", NAPI_AUTO_LENGTH, &script), napi_ok);
        napi_value result = nullptr;
        (void)napi_run_script(s.env, script, &result);
      },
      "FATAL ERROR: test_fatal::Test fatal message");
}
