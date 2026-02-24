#include "test_env.h"

extern "C" napi_value napi_register_module_v1(napi_env env, napi_value exports);

class Test49NodeCleanupHook : public FixtureTestBase {};

TEST_F(Test49NodeCleanupHook, PortedCoreFlow) {
  {
    EnvScope s(runtime_.get());
    napi_value exports = nullptr;
    ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
    (void)napi_register_module_v1(s.env, exports);
  }

  SUCCEED();
}
