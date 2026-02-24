#include "test_env.h"

extern "C" napi_value napi_register_module_v1(napi_env env, napi_value exports);

class Test23InstanceDataRefThenSet : public FixtureTestBase {};

TEST_F(Test23InstanceDataRefThenSet, PortedCoreFlow) {
  EnvScope s(runtime_.get());
  napi_value exports = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
  ASSERT_NE(napi_register_module_v1(s.env, exports), nullptr);

  void* instance_data = nullptr;
  ASSERT_EQ(napi_get_instance_data(s.env, &instance_data), napi_ok);
  ASSERT_NE(instance_data, nullptr);
}
