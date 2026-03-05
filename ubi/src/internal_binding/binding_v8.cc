#include "internal_binding/dispatch.h"

#include <cstring>
#include <string>

#include "internal_binding/helpers.h"

namespace internal_binding {

namespace {

napi_value MakeUndefined(napi_env env) {
  napi_value out = nullptr;
  napi_get_undefined(env, &out);
  return out;
}

void SetNamedInt(napi_env env, napi_value obj, const char* key, int32_t value) {
  napi_value v = nullptr;
  if (napi_create_int32(env, value, &v) == napi_ok && v != nullptr) {
    napi_set_named_property(env, obj, key, v);
  }
}

void SetNamedMethod(napi_env env, napi_value obj, const char* name, napi_callback cb) {
  napi_value fn = nullptr;
  if (napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, nullptr, &fn) == napi_ok && fn != nullptr) {
    napi_set_named_property(env, obj, name, fn);
  }
}

napi_value CreateFloat64Array(napi_env env, size_t length) {
  napi_value ab = nullptr;
  void* data = nullptr;
  if (napi_create_arraybuffer(env, length * sizeof(double), &data, &ab) != napi_ok || ab == nullptr) {
    return nullptr;
  }
  if (data != nullptr) {
    std::memset(data, 0, length * sizeof(double));
  }
  napi_value arr = nullptr;
  if (napi_create_typedarray(env, napi_float64_array, length, ab, 0, &arr) != napi_ok) return nullptr;
  return arr;
}

napi_value V8CachedDataVersionTag(napi_env env, napi_callback_info /*info*/) {
  napi_value out = nullptr;
  napi_create_uint32(env, 0, &out);
  return out != nullptr ? out : MakeUndefined(env);
}

napi_value V8SetFlagsFromString(napi_env env, napi_callback_info /*info*/) {
  return MakeUndefined(env);
}

napi_value V8StartCpuProfile(napi_env env, napi_callback_info /*info*/) {
  napi_value out = nullptr;
  napi_create_uint32(env, 0, &out);
  return out != nullptr ? out : MakeUndefined(env);
}

napi_value V8StopCpuProfile(napi_env env, napi_callback_info /*info*/) {
  return MakeUndefined(env);
}

napi_value V8IsStringOneByteRepresentation(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  bool is_one_byte = true;
  if (argc >= 1 && argv[0] != nullptr) {
    napi_valuetype t = napi_undefined;
    if (napi_typeof(env, argv[0], &t) != napi_ok || t != napi_string) {
      is_one_byte = false;
    } else {
      size_t len = 0;
      if (napi_get_value_string_utf8(env, argv[0], nullptr, 0, &len) == napi_ok) {
        std::string bytes(len + 1, '\0');
        size_t written = 0;
        if (napi_get_value_string_utf8(env, argv[0], bytes.data(), bytes.size(), &written) == napi_ok) {
          for (size_t i = 0; i < written; ++i) {
            if (static_cast<unsigned char>(bytes[i]) > 0x7F) {
              is_one_byte = false;
              break;
            }
          }
        } else {
          is_one_byte = false;
        }
      } else {
        is_one_byte = false;
      }
    }
  }
  napi_value out = nullptr;
  napi_get_boolean(env, is_one_byte, &out);
  return out != nullptr ? out : MakeUndefined(env);
}

napi_value V8Noop(napi_env env, napi_callback_info /*info*/) {
  return MakeUndefined(env);
}

napi_value V8GetCppHeapStatistics(napi_env env, napi_callback_info /*info*/) {
  napi_value out = nullptr;
  napi_create_object(env, &out);
  return out != nullptr ? out : MakeUndefined(env);
}

}  // namespace

napi_value ResolveV8(napi_env env, const ResolveOptions& /*options*/) {
  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) return Undefined(env);

  SetNamedMethod(env, out, "cachedDataVersionTag", V8CachedDataVersionTag);
  SetNamedMethod(env, out, "setFlagsFromString", V8SetFlagsFromString);
  SetNamedMethod(env, out, "startCpuProfile", V8StartCpuProfile);
  SetNamedMethod(env, out, "stopCpuProfile", V8StopCpuProfile);
  SetNamedMethod(env, out, "isStringOneByteRepresentation", V8IsStringOneByteRepresentation);
  SetNamedMethod(env, out, "updateHeapStatisticsBuffer", V8Noop);
  SetNamedMethod(env, out, "updateHeapSpaceStatisticsBuffer", V8Noop);
  SetNamedMethod(env, out, "updateHeapCodeStatisticsBuffer", V8Noop);
  SetNamedMethod(env, out, "setHeapSnapshotNearHeapLimit", V8Noop);
  SetNamedMethod(env, out, "getCppHeapStatistics", V8GetCppHeapStatistics);

  // Heap stats indices.
  SetNamedInt(env, out, "kTotalHeapSizeIndex", 0);
  SetNamedInt(env, out, "kTotalHeapSizeExecutableIndex", 1);
  SetNamedInt(env, out, "kTotalPhysicalSizeIndex", 2);
  SetNamedInt(env, out, "kTotalAvailableSize", 3);
  SetNamedInt(env, out, "kUsedHeapSizeIndex", 4);
  SetNamedInt(env, out, "kHeapSizeLimitIndex", 5);
  SetNamedInt(env, out, "kDoesZapGarbageIndex", 6);
  SetNamedInt(env, out, "kMallocedMemoryIndex", 7);
  SetNamedInt(env, out, "kPeakMallocedMemoryIndex", 8);
  SetNamedInt(env, out, "kNumberOfNativeContextsIndex", 9);
  SetNamedInt(env, out, "kNumberOfDetachedContextsIndex", 10);
  SetNamedInt(env, out, "kTotalGlobalHandlesSizeIndex", 11);
  SetNamedInt(env, out, "kUsedGlobalHandlesSizeIndex", 12);
  SetNamedInt(env, out, "kExternalMemoryIndex", 13);

  // Heap space stats indices.
  SetNamedInt(env, out, "kSpaceSizeIndex", 0);
  SetNamedInt(env, out, "kSpaceUsedSizeIndex", 1);
  SetNamedInt(env, out, "kSpaceAvailableSizeIndex", 2);
  SetNamedInt(env, out, "kPhysicalSpaceSizeIndex", 3);

  // Heap code stats indices.
  SetNamedInt(env, out, "kCodeAndMetadataSizeIndex", 0);
  SetNamedInt(env, out, "kBytecodeAndMetadataSizeIndex", 1);
  SetNamedInt(env, out, "kExternalScriptSourceSizeIndex", 2);
  SetNamedInt(env, out, "kCPUProfilerMetaDataSizeIndex", 3);

  napi_value heap_spaces = nullptr;
  napi_create_array_with_length(env, 0, &heap_spaces);
  if (heap_spaces != nullptr) napi_set_named_property(env, out, "kHeapSpaces", heap_spaces);

  napi_value heap_stats = CreateFloat64Array(env, 16);
  if (heap_stats != nullptr) napi_set_named_property(env, out, "heapStatisticsBuffer", heap_stats);
  napi_value heap_code_stats = CreateFloat64Array(env, 8);
  if (heap_code_stats != nullptr) napi_set_named_property(env, out, "heapCodeStatisticsBuffer", heap_code_stats);
  napi_value heap_space_stats = CreateFloat64Array(env, 8);
  if (heap_space_stats != nullptr) napi_set_named_property(env, out, "heapSpaceStatisticsBuffer", heap_space_stats);

  napi_value detail_level = nullptr;
  if (napi_create_object(env, &detail_level) == napi_ok && detail_level != nullptr) {
    SetNamedInt(env, detail_level, "DETAILED", 0);
    SetNamedInt(env, detail_level, "BRIEF", 1);
    napi_set_named_property(env, out, "detailLevel", detail_level);
  }

  return out;
}

}  // namespace internal_binding
