cmake_minimum_required(VERSION 3.20)
project(unode LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(UNODE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(NAPI_V8_ROOT "${UNODE_ROOT}/../napi-v8")
set(NAPI_V8_NODE_ROOT "${UNODE_ROOT}/../node")
set(UNODE_V8_MONOLITH_LIB_DEFAULT "")
if(DEFINED ENV{NAPI_V8_V8_LIBRARY})
  set(UNODE_V8_MONOLITH_LIB_DEFAULT "$ENV{NAPI_V8_V8_LIBRARY}")
elseif(DEFINED ENV{NAPI_V8_V8_MONOLITH_LIB})
  set(UNODE_V8_MONOLITH_LIB_DEFAULT "$ENV{NAPI_V8_V8_MONOLITH_LIB}")
endif()
set(UNODE_V8_MONOLITH_LIB
  "${UNODE_V8_MONOLITH_LIB_DEFAULT}"
  CACHE FILEPATH "Path to V8 runtime library (monolith or dylib)"
)
set(UNODE_V8_EXTRA_LIBS_DEFAULT "")
if(DEFINED ENV{NAPI_V8_V8_EXTRA_LIBS})
  set(UNODE_V8_EXTRA_LIBS_DEFAULT "$ENV{NAPI_V8_V8_EXTRA_LIBS}")
endif()
set(UNODE_V8_EXTRA_LIBS
  "${UNODE_V8_EXTRA_LIBS_DEFAULT}"
  CACHE STRING "Semicolon-separated extra V8 libraries (e.g. libplatform/libbase)"
)

# Build napi-v8 runtime only; node-api logic is owned by unode.
set(NAPI_V8_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${NAPI_V8_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/napi-v8")

set(NAPI_V8_UV_INCLUDE_DIR "${NAPI_V8_NODE_ROOT}/deps/uv/include")
find_library(UNODE_UV_LIB NAMES uv libuv HINTS /opt/homebrew/lib /usr/local/lib)

add_library(unode_node_api STATIC
  src/node_api_v8.cc
)

target_include_directories(unode_node_api
  PRIVATE
    "${NAPI_V8_ROOT}/src"
    "${NAPI_V8_ROOT}/include"
    "${NAPI_V8_V8_INCLUDE_DIR}"
    "${NAPI_V8_UV_INCLUDE_DIR}"
)

target_link_libraries(unode_node_api
  PUBLIC
    napi_v8
)
if(UNODE_UV_LIB)
  target_link_libraries(unode_node_api PRIVATE "${UNODE_UV_LIB}")
endif()

enable_testing()
add_subdirectory(tests)
