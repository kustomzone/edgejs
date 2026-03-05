#include "unofficial_napi.h"

#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "internal/napi_v8_env.h"
#include "node_api.h"

namespace {

constexpr int kHostDefinedOptionsId = 8;
constexpr int kHostDefinedOptionsLength = 9;

struct ContextRecord {
  napi_ref key_ref = nullptr;
  v8::Global<v8::Context> context;
  std::unique_ptr<v8::MicrotaskQueue> own_microtask_queue;
};

std::mutex g_context_mu;
std::unordered_map<napi_env, std::vector<ContextRecord>> g_context_records;
std::unordered_set<napi_env> g_context_cleanup_hooks;

struct SavedOwnProperty {
  v8::Global<v8::Name> key;
  v8::Global<v8::Value> value;
};

v8::Local<v8::String> OneByteString(v8::Isolate* isolate, const char* value) {
  return v8::String::NewFromUtf8(isolate, value, v8::NewStringType::kInternalized)
      .ToLocalChecked();
}

bool IsNullish(napi_env env, napi_value value) {
  if (value == nullptr) return true;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok) return true;
  return type == napi_undefined || type == napi_null;
}

bool CoerceToStringValue(napi_env env, napi_value input, napi_value* out) {
  if (out == nullptr) return false;
  *out = nullptr;
  if (input == nullptr) return false;
  return napi_coerce_to_string(env, input, out) == napi_ok && *out != nullptr;
}

v8::Local<v8::String> ToV8String(napi_env env, napi_value value, const char* fallback) {
  v8::Isolate* isolate = env->isolate;
  napi_value str = nullptr;
  if (!CoerceToStringValue(env, value, &str)) {
    return v8::String::NewFromUtf8(isolate, fallback, v8::NewStringType::kNormal).ToLocalChecked();
  }
  v8::Local<v8::Value> raw = napi_v8_unwrap_value(str);
  if (raw.IsEmpty() || !raw->IsString()) {
    return v8::String::NewFromUtf8(isolate, fallback, v8::NewStringType::kNormal).ToLocalChecked();
  }
  return raw.As<v8::String>();
}

bool SetNamed(v8::Local<v8::Context> context,
              v8::Local<v8::Object> target,
              const char* key,
              v8::Local<v8::Value> value) {
  return target->Set(context, OneByteString(context->GetIsolate(), key), value).FromMaybe(false);
}

bool SetSymbol(v8::Local<v8::Context> context,
               v8::Local<v8::Object> target,
               v8::Local<v8::Symbol> key,
               v8::Local<v8::Value> value) {
  if (key.IsEmpty()) return false;
  return target->Set(context, key, value).FromMaybe(false);
}

bool TryGetInternalBindingSymbol(napi_env env,
                                 const char* binding_name,
                                 const char* symbol_name,
                                 v8::Local<v8::Symbol>* out) {
  if (out == nullptr) return false;
  *out = v8::Local<v8::Symbol>();

  v8::Isolate* isolate = env->isolate;
  v8::Local<v8::Context> context = env->context();
  v8::Local<v8::Object> global = context->Global();

  v8::Local<v8::Value> internal_binding_value;
  if (!global->Get(context, OneByteString(isolate, "internalBinding")).ToLocal(&internal_binding_value) ||
      !internal_binding_value->IsFunction()) {
    return false;
  }

  v8::Local<v8::Function> internal_binding = internal_binding_value.As<v8::Function>();
  v8::Local<v8::Value> argv[1] = {OneByteString(isolate, binding_name)};
  v8::Local<v8::Value> binding_value;
  if (!internal_binding->Call(context, global, 1, argv).ToLocal(&binding_value) || !binding_value->IsObject()) {
    return false;
  }

  v8::Local<v8::Object> binding = binding_value.As<v8::Object>();
  v8::Local<v8::Value> symbols_value;
  if (!binding->Get(context, OneByteString(isolate, "privateSymbols")).ToLocal(&symbols_value) ||
      !symbols_value->IsObject()) {
    return false;
  }

  v8::Local<v8::Object> symbols = symbols_value.As<v8::Object>();
  v8::Local<v8::Value> symbol_value;
  if (!symbols->Get(context, OneByteString(isolate, symbol_name)).ToLocal(&symbol_value) || !symbol_value->IsSymbol()) {
    return false;
  }

  *out = symbol_value.As<v8::Symbol>();
  return true;
}

v8::Local<v8::PrimitiveArray> HostDefinedOptions(v8::Isolate* isolate, v8::Local<v8::Symbol> id_symbol) {
  v8::Local<v8::PrimitiveArray> out = v8::PrimitiveArray::New(isolate, kHostDefinedOptionsLength);
  out->Set(isolate, kHostDefinedOptionsId, id_symbol.IsEmpty() ? v8::Undefined(isolate) : id_symbol);
  return out;
}

bool ReadArrayBufferViewBytes(v8::Local<v8::Value> value,
                              const uint8_t** data_out,
                              size_t* size_out) {
  if (data_out == nullptr || size_out == nullptr || value.IsEmpty() || !value->IsArrayBufferView()) {
    return false;
  }
  v8::Local<v8::ArrayBufferView> view = value.As<v8::ArrayBufferView>();
  std::shared_ptr<v8::BackingStore> store = view->Buffer()->GetBackingStore();
  if (!store || store->Data() == nullptr) {
    *data_out = nullptr;
    *size_out = 0;
    return true;
  }
  *data_out = static_cast<const uint8_t*>(store->Data()) + view->ByteOffset();
  *size_out = view->ByteLength();
  return true;
}

bool CreateNodeBufferFromBytes(napi_env env, const uint8_t* data, size_t size, napi_value* out) {
  if (out == nullptr) return false;
  *out = nullptr;

  v8::Isolate* isolate = env->isolate;
  v8::Local<v8::Context> context = env->context();

  std::unique_ptr<v8::BackingStore> store = v8::ArrayBuffer::NewBackingStore(isolate, size);
  if (!store) return false;
  if (size > 0 && data != nullptr) {
    std::memcpy(store->Data(), data, size);
  }

  v8::Local<v8::ArrayBuffer> ab = v8::ArrayBuffer::New(isolate, std::move(store));
  v8::Local<v8::Uint8Array> view = v8::Uint8Array::New(ab, 0, size);

  v8::Local<v8::Object> global = context->Global();
  v8::Local<v8::Value> buffer_ctor_value;
  if (global->Get(context, OneByteString(isolate, "Buffer")).ToLocal(&buffer_ctor_value) &&
      buffer_ctor_value->IsFunction()) {
    v8::Local<v8::Object> buffer_ctor_obj = buffer_ctor_value.As<v8::Object>();
    v8::Local<v8::Value> from_value;
    if (buffer_ctor_obj->Get(context, OneByteString(isolate, "from")).ToLocal(&from_value) &&
        from_value->IsFunction()) {
      v8::Local<v8::Value> argv[1] = {view};
      v8::Local<v8::Value> buffer_out;
      if (from_value.As<v8::Function>()->Call(context, buffer_ctor_value, 1, argv).ToLocal(&buffer_out)) {
        *out = napi_v8_wrap_value(env, buffer_out);
        return *out != nullptr;
      }
    }
  }

  *out = napi_v8_wrap_value(env, view);
  return *out != nullptr;
}

bool SnapshotOwnProperties(v8::Isolate* isolate,
                           v8::Local<v8::Context> context,
                           v8::Local<v8::Object> object,
                           std::vector<SavedOwnProperty>* out) {
  if (out == nullptr) return false;
  out->clear();

  v8::Local<v8::Array> names;
  if (!object
           ->GetPropertyNames(context,
                              v8::KeyCollectionMode::kOwnOnly,
                              static_cast<v8::PropertyFilter>(v8::PropertyFilter::ALL_PROPERTIES),
                              v8::IndexFilter::kIncludeIndices,
                              v8::KeyConversionMode::kKeepNumbers)
           .ToLocal(&names)) {
    return false;
  }

  out->reserve(names->Length());
  for (uint32_t i = 0; i < names->Length(); ++i) {
    v8::Local<v8::Value> key_value;
    if (!names->Get(context, i).ToLocal(&key_value) || !key_value->IsName()) {
      continue;
    }
    v8::Local<v8::Name> key = key_value.As<v8::Name>();
    v8::Local<v8::Value> value;
    if (!object->Get(context, key).ToLocal(&value)) {
      return false;
    }
    SavedOwnProperty saved;
    saved.key.Reset(isolate, key);
    saved.value.Reset(isolate, value);
    out->push_back(std::move(saved));
  }
  return true;
}

bool RestoreOwnProperties(v8::Isolate* isolate,
                          v8::Local<v8::Context> context,
                          v8::Local<v8::Object> object,
                          const std::vector<SavedOwnProperty>& saved_properties) {
  for (const SavedOwnProperty& saved : saved_properties) {
    v8::Local<v8::Name> key = saved.key.Get(isolate);
    v8::Local<v8::Value> value = saved.value.Get(isolate);
    if (key.IsEmpty() || !object->Set(context, key, value).FromMaybe(false)) {
      return false;
    }
  }
  return true;
}

void CleanupContextRecords(void* arg) {
  napi_env env = static_cast<napi_env>(arg);

  std::lock_guard<std::mutex> lock(g_context_mu);
  g_context_cleanup_hooks.erase(env);
  auto it = g_context_records.find(env);
  if (it == g_context_records.end()) return;
  for (auto& rec : it->second) {
    if (rec.key_ref != nullptr) {
      napi_delete_reference(env, rec.key_ref);
      rec.key_ref = nullptr;
    }
    rec.context.Reset();
    rec.own_microtask_queue.reset();
  }
  g_context_records.erase(it);
}

void EnsureContextCleanupHook(napi_env env) {
  auto [it, inserted] = g_context_cleanup_hooks.emplace(env);
  if (!inserted) return;
  if (napi_add_env_cleanup_hook(env, CleanupContextRecords, env) != napi_ok) {
    g_context_cleanup_hooks.erase(it);
  }
}

ContextRecord* FindRecordByKey(napi_env env, napi_value key) {
  auto it = g_context_records.find(env);
  if (it == g_context_records.end()) return nullptr;
  for (auto& rec : it->second) {
    if (rec.key_ref == nullptr) continue;
    napi_value candidate = nullptr;
    if (napi_get_reference_value(env, rec.key_ref, &candidate) != napi_ok || candidate == nullptr) continue;
    bool same = false;
    if (napi_strict_equals(env, candidate, key, &same) == napi_ok && same) {
      return &rec;
    }
  }
  return nullptr;
}

bool StoreRecord(napi_env env,
                 napi_value key,
                 v8::Local<v8::Context> context,
                 std::unique_ptr<v8::MicrotaskQueue> own_microtask_queue) {
  EnsureContextCleanupHook(env);

  ContextRecord record;
  if (napi_create_reference(env, key, 1, &record.key_ref) != napi_ok || record.key_ref == nullptr) {
    return false;
  }
  record.context.Reset(env->isolate, context);
  record.own_microtask_queue = std::move(own_microtask_queue);
  g_context_records[env].push_back(std::move(record));
  return true;
}

bool ResolveContextFromKey(napi_env env,
                           napi_value key,
                           v8::Local<v8::Context>* context_out,
                           v8::MicrotaskQueue** microtask_queue_out) {
  if (context_out == nullptr || microtask_queue_out == nullptr) return false;
  *microtask_queue_out = nullptr;
  ContextRecord* rec = FindRecordByKey(env, key);
  if (rec == nullptr) return false;
  v8::Local<v8::Context> context = rec->context.Get(env->isolate);
  if (context.IsEmpty()) return false;
  *context_out = context;
  *microtask_queue_out = rec->own_microtask_queue ? rec->own_microtask_queue.get() : nullptr;
  return true;
}

bool CompileAsModule(v8::Isolate* isolate,
                     v8::Local<v8::Context> context,
                     v8::Local<v8::String> code,
                     v8::Local<v8::String> resource_name) {
  v8::TryCatch tc(isolate);
  v8::ScriptOrigin origin(resource_name,
                          0,
                          0,
                          true,
                          -1,
                          v8::Local<v8::Value>(),
                          false,
                          false,
                          true);
  v8::ScriptCompiler::Source source(code, origin);
  v8::Local<v8::Module> module;
  if (v8::ScriptCompiler::CompileModule(isolate, &source).ToLocal(&module)) {
    return true;
  }
  return false;
}

v8::MaybeLocal<v8::Function> CompileCjsFunction(v8::Local<v8::Context> context,
                                                v8::Local<v8::String> code,
                                                v8::Local<v8::String> filename,
                                                bool is_cjs_scope) {
  v8::Isolate* isolate = context->GetIsolate();

  v8::ScriptOrigin origin(filename,
                          0,
                          0,
                          true,
                          -1,
                          v8::Local<v8::Value>(),
                          false,
                          false,
                          false);
  v8::ScriptCompiler::Source source(code, origin);

  std::vector<v8::Local<v8::String>> params;
  if (is_cjs_scope) {
    params.emplace_back(OneByteString(isolate, "exports"));
    params.emplace_back(OneByteString(isolate, "require"));
    params.emplace_back(OneByteString(isolate, "module"));
    params.emplace_back(OneByteString(isolate, "__filename"));
    params.emplace_back(OneByteString(isolate, "__dirname"));
  }

  return v8::ScriptCompiler::CompileFunction(context,
                                             &source,
                                             params.size(),
                                             params.empty() ? nullptr : params.data(),
                                             0,
                                             nullptr,
                                             v8::ScriptCompiler::kNoCompileOptions,
                                             v8::ScriptCompiler::NoCacheReason::kNoCacheNoReason);
}

}  // namespace

extern "C" {

napi_status NAPI_CDECL unofficial_napi_contextify_make_context(
    napi_env env,
    napi_value sandbox_or_symbol,
    napi_value name,
    napi_value origin_or_undefined,
    bool allow_code_gen_strings,
    bool allow_code_gen_wasm,
    bool own_microtask_queue,
    napi_value host_defined_option_id,
    napi_value* result_out) {
  if (env == nullptr || sandbox_or_symbol == nullptr || name == nullptr || result_out == nullptr) {
    return napi_invalid_arg;
  }
  (void)allow_code_gen_wasm;

  v8::Isolate* isolate = env->isolate;
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> current = env->context();
  v8::Context::Scope current_scope(current);

  napi_valuetype sandbox_type = napi_undefined;
  if (napi_typeof(env, sandbox_or_symbol, &sandbox_type) != napi_ok) {
    return napi_invalid_arg;
  }

  const bool vanilla = sandbox_type == napi_symbol;
  if (!vanilla && sandbox_type != napi_object && sandbox_type != napi_function) {
    return napi_invalid_arg;
  }

  v8::Local<v8::Symbol> host_id_symbol;
  if (host_defined_option_id != nullptr) {
    v8::Local<v8::Value> host_raw = napi_v8_unwrap_value(host_defined_option_id);
    if (!host_raw.IsEmpty() && host_raw->IsSymbol()) {
      host_id_symbol = host_raw.As<v8::Symbol>();
    }
  }

  std::unique_ptr<v8::MicrotaskQueue> own_queue;
  v8::MicrotaskQueue* queue = nullptr;
  if (own_microtask_queue) {
    own_queue = v8::MicrotaskQueue::New(isolate, v8::MicrotasksPolicy::kExplicit);
    queue = own_queue.get();
  }

  std::vector<SavedOwnProperty> saved_properties;
  v8::Local<v8::Object> sandbox_object;
  v8::MaybeLocal<v8::Value> maybe_global_object;
  if (!vanilla) {
    v8::Local<v8::Value> sandbox_value = napi_v8_unwrap_value(sandbox_or_symbol);
    if (sandbox_value.IsEmpty() || !sandbox_value->IsObject()) return napi_invalid_arg;
    sandbox_object = sandbox_value.As<v8::Object>();
    if (!SnapshotOwnProperties(isolate, current, sandbox_object, &saved_properties)) {
      return napi_pending_exception;
    }
    maybe_global_object = sandbox_value;
  }

  v8::Local<v8::Context> context = v8::Context::New(isolate,
                                                    nullptr,
                                                    v8::MaybeLocal<v8::ObjectTemplate>(),
                                                    maybe_global_object,
                                                    v8::DeserializeInternalFieldsCallback(),
                                                    queue);

  if (context.IsEmpty()) {
    return napi_pending_exception;
  }

  context->SetSecurityToken(current->GetSecurityToken());
  context->AllowCodeGenerationFromStrings(allow_code_gen_strings);

  v8::Local<v8::Object> key_object;
  if (vanilla) {
    key_object = context->Global();
  } else {
    key_object = sandbox_object;
    if (!RestoreOwnProperties(isolate, context, key_object, saved_properties)) {
      return napi_pending_exception;
    }
  }

  napi_value key_napi = napi_v8_wrap_value(env, key_object);
  if (key_napi == nullptr) return napi_generic_failure;

  {
    std::lock_guard<std::mutex> lock(g_context_mu);
    if (FindRecordByKey(env, key_napi) != nullptr) {
      return napi_invalid_arg;
    }
    if (!StoreRecord(env, key_napi, context, std::move(own_queue))) {
      return napi_generic_failure;
    }
  }

  // Align with node-lib internal/vm.js isContext() checks in Ubi.
  v8::Local<v8::Context> property_context = vanilla ? context : current;
  v8::Local<v8::Symbol> context_symbol;
  if (TryGetInternalBindingSymbol(env, "util", "contextify_context_private_symbol", &context_symbol)) {
    SetSymbol(property_context, key_object, context_symbol, key_object);
  }

  v8::Local<v8::Symbol> host_symbol;
  if (TryGetInternalBindingSymbol(env, "util", "host_defined_option_symbol", &host_symbol)) {
    SetSymbol(property_context,
              key_object,
              host_symbol,
              host_id_symbol.IsEmpty() ? v8::Undefined(isolate) : host_id_symbol.As<v8::Value>());
  }

  *result_out = key_napi;
  return napi_ok;
}

napi_status NAPI_CDECL unofficial_napi_contextify_run_script(
    napi_env env,
    napi_value sandbox_or_null,
    napi_value source,
    napi_value filename,
    int32_t line_offset,
    int32_t column_offset,
    int64_t timeout,
    bool display_errors,
    bool break_on_sigint,
    bool break_on_first_line,
    napi_value host_defined_option_id,
    napi_value* result_out) {
  if (env == nullptr || source == nullptr || filename == nullptr || result_out == nullptr) {
    return napi_invalid_arg;
  }
  (void)timeout;
  (void)display_errors;
  (void)break_on_sigint;
  (void)break_on_first_line;

  v8::Isolate* isolate = env->isolate;
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> current = env->context();
  v8::Context::Scope current_scope(current);

  v8::Local<v8::Context> target_context = current;
  v8::MicrotaskQueue* target_queue = nullptr;

  if (!IsNullish(env, sandbox_or_null)) {
    std::lock_guard<std::mutex> lock(g_context_mu);
    if (!ResolveContextFromKey(env, sandbox_or_null, &target_context, &target_queue)) {
      return napi_invalid_arg;
    }
  }

  v8::Local<v8::String> code = ToV8String(env, source, "");
  v8::Local<v8::String> filename_str = ToV8String(env, filename, "[eval]");

  v8::Local<v8::Symbol> host_id_symbol;
  if (host_defined_option_id != nullptr) {
    v8::Local<v8::Value> host_raw = napi_v8_unwrap_value(host_defined_option_id);
    if (!host_raw.IsEmpty() && host_raw->IsSymbol()) {
      host_id_symbol = host_raw.As<v8::Symbol>();
    }
  }

  v8::TryCatch try_catch(isolate);
  v8::Context::Scope scope(target_context);
  v8::ScriptOrigin origin(filename_str,
                          line_offset,
                          column_offset,
                          true,
                          -1,
                          v8::Local<v8::Value>(),
                          false,
                          false,
                          false,
                          HostDefinedOptions(isolate, host_id_symbol));
  v8::Local<v8::Script> script;
  if (!v8::Script::Compile(target_context, code, &origin).ToLocal(&script)) {
    if (try_catch.HasCaught() && !try_catch.HasTerminated()) {
      try_catch.ReThrow();
      return napi_pending_exception;
    }
    return napi_generic_failure;
  }

  v8::Local<v8::Value> result;
  if (!script->Run(target_context).ToLocal(&result)) {
    if (try_catch.HasCaught() && !try_catch.HasTerminated()) {
      try_catch.ReThrow();
      return napi_pending_exception;
    }
    return napi_generic_failure;
  }

  if (target_queue != nullptr) {
    target_queue->PerformCheckpoint(isolate);
  }

  *result_out = napi_v8_wrap_value(env, result);
  return *result_out == nullptr ? napi_generic_failure : napi_ok;
}

napi_status NAPI_CDECL unofficial_napi_contextify_dispose_context(
    napi_env env,
    napi_value sandbox_or_context_global) {
  if (env == nullptr || sandbox_or_context_global == nullptr) return napi_invalid_arg;

  std::lock_guard<std::mutex> lock(g_context_mu);
  auto it = g_context_records.find(env);
  if (it == g_context_records.end()) return napi_ok;

  auto& records = it->second;
  for (size_t i = 0; i < records.size(); ++i) {
    ContextRecord& rec = records[i];
    if (rec.key_ref == nullptr) continue;
    napi_value candidate = nullptr;
    if (napi_get_reference_value(env, rec.key_ref, &candidate) != napi_ok || candidate == nullptr) continue;
    bool same = false;
    if (napi_strict_equals(env, candidate, sandbox_or_context_global, &same) != napi_ok || !same) continue;
    napi_delete_reference(env, rec.key_ref);
    rec.key_ref = nullptr;
    rec.context.Reset();
    rec.own_microtask_queue.reset();
    records.erase(records.begin() + static_cast<long>(i));
    break;
  }

  if (records.empty()) {
    g_context_records.erase(it);
  }
  return napi_ok;
}

napi_status NAPI_CDECL unofficial_napi_contextify_compile_function(
    napi_env env,
    napi_value code,
    napi_value filename,
    int32_t line_offset,
    int32_t column_offset,
    napi_value cached_data_or_undefined,
    bool produce_cached_data,
    napi_value parsing_context_or_undefined,
    napi_value context_extensions_or_undefined,
    napi_value params_or_undefined,
    napi_value host_defined_option_id,
    napi_value* result_out) {
  if (env == nullptr || code == nullptr || filename == nullptr || result_out == nullptr) {
    return napi_invalid_arg;
  }

  v8::Isolate* isolate = env->isolate;
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> current = env->context();
  v8::Context::Scope current_scope(current);

  v8::Local<v8::Context> parsing_context = current;
  if (!IsNullish(env, parsing_context_or_undefined)) {
    std::lock_guard<std::mutex> lock(g_context_mu);
    v8::MicrotaskQueue* ignored = nullptr;
    if (!ResolveContextFromKey(env, parsing_context_or_undefined, &parsing_context, &ignored)) {
      return napi_invalid_arg;
    }
  }

  v8::Local<v8::String> code_str = ToV8String(env, code, "");
  v8::Local<v8::String> filename_str = ToV8String(env, filename, "");

  v8::Local<v8::Symbol> host_id_symbol;
  if (host_defined_option_id != nullptr) {
    v8::Local<v8::Value> host_raw = napi_v8_unwrap_value(host_defined_option_id);
    if (!host_raw.IsEmpty() && host_raw->IsSymbol()) {
      host_id_symbol = host_raw.As<v8::Symbol>();
    }
  }

  v8::ScriptCompiler::CachedData* cached_data = nullptr;
  if (!IsNullish(env, cached_data_or_undefined)) {
    v8::Local<v8::Value> cached_data_value = napi_v8_unwrap_value(cached_data_or_undefined);
    if (cached_data_value.IsEmpty() || !cached_data_value->IsArrayBufferView()) {
      return napi_invalid_arg;
    }
    v8::Local<v8::ArrayBufferView> cached_data_view = cached_data_value.As<v8::ArrayBufferView>();
    uint8_t* ptr = static_cast<uint8_t*>(cached_data_view->Buffer()->Data());
    cached_data = new v8::ScriptCompiler::CachedData(
        ptr + cached_data_view->ByteOffset(), cached_data_view->ByteLength());
  }

  std::vector<v8::Local<v8::Object>> context_extensions;
  if (!IsNullish(env, context_extensions_or_undefined)) {
    v8::Local<v8::Value> value = napi_v8_unwrap_value(context_extensions_or_undefined);
    if (value.IsEmpty() || !value->IsArray()) return napi_invalid_arg;
    v8::Local<v8::Array> array = value.As<v8::Array>();
    context_extensions.reserve(array->Length());
    for (uint32_t i = 0; i < array->Length(); ++i) {
      v8::Local<v8::Value> item;
      if (!array->Get(current, i).ToLocal(&item) || !item->IsObject()) return napi_invalid_arg;
      context_extensions.push_back(item.As<v8::Object>());
    }
  }

  std::vector<v8::Local<v8::String>> params;
  if (!IsNullish(env, params_or_undefined)) {
    v8::Local<v8::Value> value = napi_v8_unwrap_value(params_or_undefined);
    if (value.IsEmpty() || !value->IsArray()) return napi_invalid_arg;
    v8::Local<v8::Array> array = value.As<v8::Array>();
    params.reserve(array->Length());
    for (uint32_t i = 0; i < array->Length(); ++i) {
      v8::Local<v8::Value> item;
      if (!array->Get(current, i).ToLocal(&item) || !item->IsString()) return napi_invalid_arg;
      params.push_back(item.As<v8::String>());
    }
  }

  v8::ScriptOrigin origin(filename_str,
                          line_offset,
                          column_offset,
                          true,
                          -1,
                          v8::Local<v8::Value>(),
                          false,
                          false,
                          false,
                          HostDefinedOptions(isolate, host_id_symbol));

  v8::ScriptCompiler::Source source_obj(code_str, origin, cached_data);
  v8::ScriptCompiler::CompileOptions options = source_obj.GetCachedData() != nullptr
                                                   ? v8::ScriptCompiler::kConsumeCodeCache
                                                   : v8::ScriptCompiler::kNoCompileOptions;

  v8::TryCatch try_catch(isolate);
  v8::Context::Scope parsing_scope(parsing_context);
  v8::MaybeLocal<v8::Function> maybe_fn = v8::ScriptCompiler::CompileFunction(
      parsing_context,
      &source_obj,
      params.size(),
      params.empty() ? nullptr : params.data(),
      context_extensions.size(),
      context_extensions.empty() ? nullptr : context_extensions.data(),
      options,
      v8::ScriptCompiler::NoCacheReason::kNoCacheNoReason);

  v8::Local<v8::Function> fn;
  if (!maybe_fn.ToLocal(&fn)) {
    if (try_catch.HasCaught() && !try_catch.HasTerminated()) {
      try_catch.ReThrow();
      return napi_pending_exception;
    }
    return napi_generic_failure;
  }

  v8::Local<v8::Symbol> host_symbol;
  if (TryGetInternalBindingSymbol(env, "util", "host_defined_option_symbol", &host_symbol)) {
    SetSymbol(current,
              fn.As<v8::Object>(),
              host_symbol,
              host_id_symbol.IsEmpty() ? v8::Undefined(isolate) : host_id_symbol.As<v8::Value>());
  }

  v8::Local<v8::Object> out = v8::Object::New(isolate);
  if (!SetNamed(current, out, "function", fn)) return napi_generic_failure;

  v8::ScriptOrigin fn_origin = fn->GetScriptOrigin();
  if (!SetNamed(current, out, "sourceURL", fn_origin.ResourceName())) return napi_generic_failure;
  if (!SetNamed(current, out, "sourceMapURL", fn_origin.SourceMapUrl())) return napi_generic_failure;

  if (options == v8::ScriptCompiler::kConsumeCodeCache && source_obj.GetCachedData() != nullptr) {
    if (!SetNamed(current,
                  out,
                  "cachedDataRejected",
                  v8::Boolean::New(isolate, source_obj.GetCachedData()->rejected))) {
      return napi_generic_failure;
    }
  }

  std::unique_ptr<v8::ScriptCompiler::CachedData> produced_cache;
  if (produce_cached_data) {
    produced_cache.reset(v8::ScriptCompiler::CreateCodeCacheForFunction(fn));
    if (!SetNamed(current, out, "cachedDataProduced", v8::Boolean::New(isolate, produced_cache != nullptr))) {
      return napi_generic_failure;
    }
    if (produced_cache != nullptr) {
      napi_value cache_buffer = nullptr;
      if (!CreateNodeBufferFromBytes(env,
                                     produced_cache->data,
                                     static_cast<size_t>(produced_cache->length),
                                     &cache_buffer) ||
          cache_buffer == nullptr) {
        return napi_generic_failure;
      }
      v8::Local<v8::Value> wrapped_cache = napi_v8_unwrap_value(cache_buffer);
      if (!SetNamed(current, out, "cachedData", wrapped_cache)) return napi_generic_failure;
    }
  }

  *result_out = napi_v8_wrap_value(env, out);
  return *result_out == nullptr ? napi_generic_failure : napi_ok;
}

napi_status NAPI_CDECL unofficial_napi_contextify_compile_function_for_cjs_loader(
    napi_env env,
    napi_value code,
    napi_value filename,
    bool is_sea_main,
    bool should_detect_module,
    napi_value* result_out) {
  if (env == nullptr || code == nullptr || filename == nullptr || result_out == nullptr) return napi_invalid_arg;
  (void)is_sea_main;

  v8::Isolate* isolate = env->isolate;
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context = env->context();
  v8::Context::Scope context_scope(context);

  v8::Local<v8::String> code_str = ToV8String(env, code, "");
  v8::Local<v8::String> filename_str = ToV8String(env, filename, "[eval]");

  v8::Local<v8::Function> fn;
  v8::Local<v8::Value> cjs_exception;
  bool cjs_ok = false;
  {
    v8::TryCatch tc(isolate);
    cjs_ok = CompileCjsFunction(context, code_str, filename_str, true).ToLocal(&fn);
    if (!cjs_ok && tc.HasCaught()) {
      cjs_exception = tc.Exception();
    }
  }

  bool can_parse_as_esm = false;
  if (!cjs_ok) {
    can_parse_as_esm = CompileAsModule(isolate, context, code_str, filename_str);
    if (!can_parse_as_esm || !should_detect_module) {
      if (!cjs_exception.IsEmpty()) isolate->ThrowException(cjs_exception);
      return cjs_exception.IsEmpty() ? napi_generic_failure : napi_pending_exception;
    }
  }

  v8::Local<v8::Object> out = v8::Object::New(isolate);
  if (!SetNamed(context, out, "cachedDataRejected", v8::Boolean::New(isolate, false)) ||
      !SetNamed(context, out, "canParseAsESM", v8::Boolean::New(isolate, can_parse_as_esm))) {
    return napi_generic_failure;
  }

  if (cjs_ok) {
    v8::ScriptOrigin origin = fn->GetScriptOrigin();
    if (!SetNamed(context, out, "sourceMapURL", origin.SourceMapUrl()) ||
        !SetNamed(context, out, "sourceURL", origin.ResourceName()) ||
        !SetNamed(context, out, "function", fn)) {
      return napi_generic_failure;
    }
  } else {
    if (!SetNamed(context, out, "sourceMapURL", v8::Undefined(isolate)) ||
        !SetNamed(context, out, "sourceURL", v8::Undefined(isolate)) ||
        !SetNamed(context, out, "function", v8::Undefined(isolate))) {
      return napi_generic_failure;
    }
  }

  *result_out = napi_v8_wrap_value(env, out);
  return *result_out == nullptr ? napi_generic_failure : napi_ok;
}

napi_status NAPI_CDECL unofficial_napi_contextify_contains_module_syntax(
    napi_env env,
    napi_value code,
    napi_value filename,
    napi_value resource_name_or_undefined,
    bool cjs_var_in_scope,
    bool* result_out) {
  if (env == nullptr || code == nullptr || filename == nullptr || result_out == nullptr) return napi_invalid_arg;

  v8::Isolate* isolate = env->isolate;
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context = env->context();
  v8::Context::Scope context_scope(context);

  v8::Local<v8::String> code_str = ToV8String(env, code, "");
  v8::Local<v8::String> filename_str = ToV8String(env, filename, "[eval]");
  v8::Local<v8::String> resource_name = filename_str;
  if (!IsNullish(env, resource_name_or_undefined)) {
    resource_name = ToV8String(env, resource_name_or_undefined, "[eval]");
  }

  {
    v8::TryCatch tc(isolate);
    v8::Local<v8::Function> fn;
    if (CompileCjsFunction(context, code_str, filename_str, cjs_var_in_scope).ToLocal(&fn)) {
      *result_out = false;
      return napi_ok;
    }
  }

  *result_out = CompileAsModule(isolate, context, code_str, resource_name);
  return napi_ok;
}

napi_status NAPI_CDECL unofficial_napi_contextify_create_cached_data(
    napi_env env,
    napi_value code,
    napi_value filename,
    int32_t line_offset,
    int32_t column_offset,
    napi_value host_defined_option_id,
    napi_value* cached_data_buffer_out) {
  if (env == nullptr || code == nullptr || filename == nullptr || cached_data_buffer_out == nullptr) {
    return napi_invalid_arg;
  }

  v8::Isolate* isolate = env->isolate;
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context = env->context();
  v8::Context::Scope context_scope(context);

  v8::Local<v8::String> code_str = ToV8String(env, code, "");
  v8::Local<v8::String> filename_str = ToV8String(env, filename, "[eval]");

  v8::Local<v8::Symbol> host_id_symbol;
  if (host_defined_option_id != nullptr) {
    v8::Local<v8::Value> host_raw = napi_v8_unwrap_value(host_defined_option_id);
    if (!host_raw.IsEmpty() && host_raw->IsSymbol()) {
      host_id_symbol = host_raw.As<v8::Symbol>();
    }
  }

  v8::ScriptOrigin origin(filename_str,
                          line_offset,
                          column_offset,
                          true,
                          -1,
                          v8::Local<v8::Value>(),
                          false,
                          false,
                          false,
                          HostDefinedOptions(isolate, host_id_symbol));
  v8::ScriptCompiler::Source source_obj(code_str, origin);

  v8::TryCatch try_catch(isolate);
  v8::MaybeLocal<v8::UnboundScript> maybe_script =
      v8::ScriptCompiler::CompileUnboundScript(isolate,
                                               &source_obj,
                                               v8::ScriptCompiler::kNoCompileOptions,
                                               v8::ScriptCompiler::NoCacheReason::kNoCacheNoReason);
  v8::Local<v8::UnboundScript> script;
  if (!maybe_script.ToLocal(&script)) {
    if (try_catch.HasCaught() && !try_catch.HasTerminated()) {
      try_catch.ReThrow();
      return napi_pending_exception;
    }
    return napi_generic_failure;
  }

  std::unique_ptr<v8::ScriptCompiler::CachedData> cache(v8::ScriptCompiler::CreateCodeCache(script));
  const uint8_t* bytes = cache ? cache->data : nullptr;
  const size_t size = cache ? static_cast<size_t>(cache->length) : 0;

  if (!CreateNodeBufferFromBytes(env, bytes, size, cached_data_buffer_out) || *cached_data_buffer_out == nullptr) {
    return napi_generic_failure;
  }
  return napi_ok;
}

}  // extern "C"
