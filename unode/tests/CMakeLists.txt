include(GoogleTest)

set(NAPI_V8_GTEST_ROOT "${NAPI_V8_NODE_ROOT}/deps/googletest")
set(NAPI_V8_UV_INCLUDE_DIR "${NAPI_V8_NODE_ROOT}/deps/uv/include")

if(UNODE_V8_MONOLITH_LIB)
  add_library(unode_gtest STATIC
    "${NAPI_V8_GTEST_ROOT}/src/gtest-all.cc"
    "${NAPI_V8_GTEST_ROOT}/src/gtest_main.cc"
  )

  target_include_directories(unode_gtest
    PRIVATE
      "${NAPI_V8_GTEST_ROOT}"
      "${NAPI_V8_GTEST_ROOT}/include"
      "${UNODE_ROOT}/tests/ported/js-native-api"
      "${UNODE_ROOT}/tests/runners"
      "${NAPI_V8_ROOT}/include"
      "${NAPI_V8_V8_INCLUDE_DIR}"
  )

  function(add_unode_node_api_test target_name)
    add_executable(${target_name} ${ARGN})
    target_include_directories(${target_name}
      PRIVATE
        "${NAPI_V8_GTEST_ROOT}"
        "${NAPI_V8_GTEST_ROOT}/include"
        "${UNODE_ROOT}/tests/ported/js-native-api"
        "${UNODE_ROOT}/tests/runners"
        "${NAPI_V8_ROOT}/include"
        "${NAPI_V8_V8_INCLUDE_DIR}"
        "${NAPI_V8_UV_INCLUDE_DIR}"
    )
    target_link_libraries(${target_name}
      PRIVATE
        unode_gtest
        napi_v8
        unode_node_api
        "${UNODE_V8_MONOLITH_LIB}"
    )
    target_compile_definitions(${target_name}
      PRIVATE
        NAPI_V8_NODE_ROOT_PATH="${NAPI_V8_NODE_ROOT}"
        NAPI_V8_ROOT_PATH="${UNODE_ROOT}"
    )
    if(NAPI_V8_V8_DEFINES)
      target_compile_definitions(${target_name} PRIVATE ${NAPI_V8_V8_DEFINES})
    endif()
    if(UNODE_V8_EXTRA_LIBS)
      target_link_libraries(${target_name} PRIVATE ${UNODE_V8_EXTRA_LIBS})
    endif()
    get_filename_component(NAPI_V8_V8_LIB_DIR "${UNODE_V8_MONOLITH_LIB}" DIRECTORY)
    set_target_properties(${target_name} PROPERTIES
      BUILD_RPATH "${NAPI_V8_V8_LIB_DIR}"
      INSTALL_RPATH "${NAPI_V8_V8_LIB_DIR}"
    )
    gtest_discover_tests(
      ${target_name}
      DISCOVERY_TIMEOUT 30
      TEST_PREFIX "unode.${target_name}."
    )
  endfunction()

  add_unode_node_api_test(unode_test_20_node_general
    "${UNODE_ROOT}/tests/runners/test_20_node_general.cc"
    "${UNODE_ROOT}/tests/ported/node-api/test_general/test_general.c"
  )
  add_unode_node_api_test(unode_test_21_general
    "${UNODE_ROOT}/tests/runners/test_21_general.cc"
    "${UNODE_ROOT}/tests/ported/js-native-api/test_general/test_general.c"
  )
  target_compile_definitions(unode_test_21_general PRIVATE NAPI_EXPERIMENTAL)
  add_unode_node_api_test(unode_test_22_node_exception
    "${UNODE_ROOT}/tests/runners/test_22_node_exception.cc"
    "${UNODE_ROOT}/tests/ported/node-api/test_exception/test_exception.c"
  )
  add_unode_node_api_test(unode_test_23_instance_data_ref_then_set
    "${UNODE_ROOT}/tests/runners/test_23_instance_data_ref_then_set.cc"
    "${UNODE_ROOT}/tests/ported/node-api/test_instance_data/addon.c"
    "${UNODE_ROOT}/tests/ported/node-api/test_instance_data/test_ref_then_set.c"
  )
  add_unode_node_api_test(unode_test_24_instance_data_set_then_ref
    "${UNODE_ROOT}/tests/runners/test_24_instance_data_set_then_ref.cc"
    "${UNODE_ROOT}/tests/ported/node-api/test_instance_data/addon.c"
    "${UNODE_ROOT}/tests/ported/node-api/test_instance_data/test_set_then_ref.c"
  )
  add_unode_node_api_test(unode_test_28_node_instance_data
    "${UNODE_ROOT}/tests/runners/test_28_node_instance_data.cc"
    "${UNODE_ROOT}/tests/ported/node-api/test_instance_data/test_instance_data.c"
  )
  add_unode_node_api_test(unode_test_38_finalizer
    "${UNODE_ROOT}/tests/runners/test_38_finalizer.cc"
    "${UNODE_ROOT}/tests/ported/js-native-api/test_finalizer/test_finalizer.c"
  )
  target_compile_definitions(unode_test_38_finalizer PRIVATE NAPI_EXPERIMENTAL NODE_API_EXPERIMENTAL_NO_WARNING)
  add_unode_node_api_test(unode_test_42_node_async
    "${UNODE_ROOT}/tests/runners/test_42_node_async.cc"
    "${UNODE_ROOT}/tests/ported/node-api/test_async/test_async.c"
  )
  add_unode_node_api_test(unode_test_43_node_hello_world
    "${UNODE_ROOT}/tests/runners/test_43_node_hello_world.cc"
    "${UNODE_ROOT}/tests/ported/node-api/1_hello_world/binding.c"
  )
  add_unode_node_api_test(unode_test_44_node_async_cleanup_hook
    "${UNODE_ROOT}/tests/runners/test_44_node_async_cleanup_hook.cc"
    "${UNODE_ROOT}/tests/ported/node-api/test_async_cleanup_hook/binding.c"
  )
  add_unode_node_api_test(unode_test_45_node_async_context
    "${UNODE_ROOT}/tests/runners/test_45_node_async_context.cc"
    "${UNODE_ROOT}/tests/ported/node-api/test_async_context/binding.c"
  )
  add_unode_node_api_test(unode_test_46_node_buffer
    "${UNODE_ROOT}/tests/runners/test_46_node_buffer.cc"
    "${UNODE_ROOT}/tests/ported/node-api/test_buffer/test_buffer.c"
  )
  target_compile_definitions(unode_test_46_node_buffer PRIVATE NAPI_VERSION=10)
  add_unode_node_api_test(unode_test_47_node_buffer_finalizer
    "${UNODE_ROOT}/tests/runners/test_47_node_buffer_finalizer.cc"
    "${UNODE_ROOT}/tests/ported/node-api/test_buffer/test_finalizer.c"
  )
  add_unode_node_api_test(unode_test_48_node_callback_scope
    "${UNODE_ROOT}/tests/runners/test_48_node_callback_scope.cc"
    "${UNODE_ROOT}/tests/ported/node-api/test_callback_scope/binding.c"
  )
  add_unode_node_api_test(unode_test_49_node_cleanup_hook
    "${UNODE_ROOT}/tests/runners/test_49_node_cleanup_hook.cc"
    "${UNODE_ROOT}/tests/ported/node-api/test_cleanup_hook/binding.c"
  )
  add_unode_node_api_test(unode_test_50_node_env_teardown_gc
    "${UNODE_ROOT}/tests/runners/test_50_node_env_teardown_gc.cc"
    "${UNODE_ROOT}/tests/ported/node-api/test_env_teardown_gc/binding.c"
  )
  add_unode_node_api_test(unode_test_51_node_fatal
    "${UNODE_ROOT}/tests/runners/test_51_node_fatal.cc"
    "${UNODE_ROOT}/tests/ported/node-api/test_fatal/test_fatal.c"
  )
  add_unode_node_api_test(unode_test_52_node_fatal_exception
    "${UNODE_ROOT}/tests/runners/test_52_node_fatal_exception.cc"
    "${UNODE_ROOT}/tests/ported/node-api/test_fatal_exception/test_fatal_exception.c"
  )
  add_unode_node_api_test(unode_test_53_node_init_order
    "${UNODE_ROOT}/tests/runners/test_53_node_init_order.cc"
    "${UNODE_ROOT}/tests/ported/node-api/test_init_order/test_init_order.cc"
  )
  add_unode_node_api_test(unode_test_54_node_make_callback
    "${UNODE_ROOT}/tests/runners/test_54_node_make_callback.cc"
    "${UNODE_ROOT}/tests/ported/node-api/test_make_callback/binding.c"
  )
  add_unode_node_api_test(unode_test_55_node_make_callback_recurse
    "${UNODE_ROOT}/tests/runners/test_55_node_make_callback_recurse.cc"
    "${UNODE_ROOT}/tests/ported/node-api/test_make_callback_recurse/binding.c"
  )
  add_unode_node_api_test(unode_test_56_node_reference_by_api_v9
    "${UNODE_ROOT}/tests/runners/test_56_node_reference_by_api_v9.cc"
    "${UNODE_ROOT}/tests/ported/node-api/test_reference_by_node_api_version/test_reference_by_node_api_version.c"
  )
  target_compile_definitions(unode_test_56_node_reference_by_api_v9 PRIVATE NAPI_VERSION=9)
  add_unode_node_api_test(unode_test_57_node_reference_by_api_v10
    "${UNODE_ROOT}/tests/runners/test_57_node_reference_by_api_v10.cc"
    "${UNODE_ROOT}/tests/ported/node-api/test_reference_by_node_api_version/test_reference_by_node_api_version.c"
  )
  target_compile_definitions(unode_test_57_node_reference_by_api_v10 PRIVATE NAPI_VERSION=10)
  add_unode_node_api_test(unode_test_58_node_threadsafe_function
    "${UNODE_ROOT}/tests/runners/test_58_node_threadsafe_function.cc"
    "${UNODE_ROOT}/tests/ported/node-api/test_threadsafe_function/binding.c"
  )
  add_unode_node_api_test(unode_test_59_node_threadsafe_function_shutdown
    "${UNODE_ROOT}/tests/runners/test_59_node_threadsafe_function_shutdown.cc"
    "${UNODE_ROOT}/tests/ported/node-api/test_threadsafe_function_shutdown/binding.cc"
  )
  add_unode_node_api_test(unode_test_60_node_uv_loop
    "${UNODE_ROOT}/tests/runners/test_60_node_uv_loop.cc"
    "${UNODE_ROOT}/tests/ported/node-api/test_uv_loop/test_uv_loop.cc"
  )
  add_unode_node_api_test(unode_test_61_node_uv_threadpool_size
    "${UNODE_ROOT}/tests/runners/test_61_node_uv_threadpool_size.cc"
    "${UNODE_ROOT}/tests/ported/node-api/test_uv_threadpool_size/test_uv_threadpool_size.c"
  )
  add_unode_node_api_test(unode_test_62_node_worker_buffer_callback
    "${UNODE_ROOT}/tests/runners/test_62_node_worker_buffer_callback.cc"
    "${UNODE_ROOT}/tests/ported/node-api/test_worker_buffer_callback/test_worker_buffer_callback.c"
  )
  add_unode_node_api_test(unode_test_63_node_worker_terminate
    "${UNODE_ROOT}/tests/runners/test_63_node_worker_terminate.cc"
    "${UNODE_ROOT}/tests/ported/node-api/test_worker_terminate/test_worker_terminate.c"
  )
  add_unode_node_api_test(unode_test_64_node_worker_terminate_finalization
    "${UNODE_ROOT}/tests/runners/test_64_node_worker_terminate_finalization.cc"
    "${UNODE_ROOT}/tests/ported/node-api/test_worker_terminate_finalization/test_worker_terminate_finalization.c"
  )

  if(UNODE_UV_LIB)
    target_link_libraries(unode_test_42_node_async PRIVATE "${UNODE_UV_LIB}")
    target_link_libraries(unode_test_44_node_async_cleanup_hook PRIVATE "${UNODE_UV_LIB}")
    target_link_libraries(unode_test_28_node_instance_data PRIVATE "${UNODE_UV_LIB}")
    target_link_libraries(unode_test_46_node_buffer PRIVATE "${UNODE_UV_LIB}")
    target_link_libraries(unode_test_47_node_buffer_finalizer PRIVATE "${UNODE_UV_LIB}")
    target_link_libraries(unode_test_48_node_callback_scope PRIVATE "${UNODE_UV_LIB}")
    target_link_libraries(unode_test_51_node_fatal PRIVATE "${UNODE_UV_LIB}")
    target_link_libraries(unode_test_58_node_threadsafe_function PRIVATE "${UNODE_UV_LIB}")
    target_link_libraries(unode_test_59_node_threadsafe_function_shutdown PRIVATE "${UNODE_UV_LIB}")
    target_link_libraries(unode_test_60_node_uv_loop PRIVATE "${UNODE_UV_LIB}")
    target_link_libraries(unode_test_61_node_uv_threadpool_size PRIVATE "${UNODE_UV_LIB}")
    target_link_libraries(unode_test_64_node_worker_terminate_finalization PRIVATE "${UNODE_UV_LIB}")
  endif()
else()
  message(STATUS "Skipping unode node-api tests: set NAPI_V8_V8_LIBRARY to link V8 runtime")
endif()
