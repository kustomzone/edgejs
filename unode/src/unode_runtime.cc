#include "unode_runtime.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string ReadTextFile(const char* path) {
  if (path == nullptr || path[0] == '\0') {
    return "";
  }
  std::ifstream in(path);
  if (!in.is_open()) {
    return "";
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string StatusToString(napi_status status) {
  switch (status) {
    case napi_ok:
      return "napi_ok";
    case napi_invalid_arg:
      return "napi_invalid_arg";
    case napi_object_expected:
      return "napi_object_expected";
    case napi_string_expected:
      return "napi_string_expected";
    case napi_name_expected:
      return "napi_name_expected";
    case napi_function_expected:
      return "napi_function_expected";
    case napi_number_expected:
      return "napi_number_expected";
    case napi_boolean_expected:
      return "napi_boolean_expected";
    case napi_array_expected:
      return "napi_array_expected";
    case napi_generic_failure:
      return "napi_generic_failure";
    case napi_pending_exception:
      return "napi_pending_exception";
    case napi_cancelled:
      return "napi_cancelled";
    case napi_escape_called_twice:
      return "napi_escape_called_twice";
    case napi_handle_scope_mismatch:
      return "napi_handle_scope_mismatch";
    case napi_callback_scope_mismatch:
      return "napi_callback_scope_mismatch";
    case napi_queue_full:
      return "napi_queue_full";
    case napi_closing:
      return "napi_closing";
    case napi_bigint_expected:
      return "napi_bigint_expected";
    case napi_date_expected:
      return "napi_date_expected";
    case napi_arraybuffer_expected:
      return "napi_arraybuffer_expected";
    case napi_detachable_arraybuffer_expected:
      return "napi_detachable_arraybuffer_expected";
    case napi_would_deadlock:
      return "napi_would_deadlock";
    case napi_no_external_buffers_allowed:
      return "napi_no_external_buffers_allowed";
    case napi_cannot_run_js:
      return "napi_cannot_run_js";
    default:
      return "napi_unknown_error";
  }
}

std::string GetAndClearPendingException(napi_env env) {
  bool pending = false;
  if (napi_is_exception_pending(env, &pending) != napi_ok || !pending) {
    return "";
  }

  napi_value exception = nullptr;
  if (napi_get_and_clear_last_exception(env, &exception) != napi_ok || exception == nullptr) {
    return "";
  }

  napi_value exception_string = nullptr;
  if (napi_coerce_to_string(env, exception, &exception_string) != napi_ok || exception_string == nullptr) {
    return "";
  }

  size_t length = 0;
  if (napi_get_value_string_utf8(env, exception_string, nullptr, 0, &length) != napi_ok) {
    return "";
  }

  std::vector<char> buffer(length + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, exception_string, buffer.data(), buffer.size(), &copied) != napi_ok) {
    return "";
  }
  return std::string(buffer.data(), copied);
}

napi_value ConsoleLogCallback(napi_env env, napi_callback_info info) {
  size_t argc = 8;
  napi_value args[8] = {nullptr};
  napi_status status = napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (status == napi_ok) {
    for (size_t i = 0; i < argc; ++i) {
      if (i > 0) {
        std::cout << " ";
      }
      napi_value string_value = nullptr;
      if (napi_coerce_to_string(env, args[i], &string_value) != napi_ok || string_value == nullptr) {
        continue;
      }
      size_t length = 0;
      if (napi_get_value_string_utf8(env, string_value, nullptr, 0, &length) != napi_ok) {
        continue;
      }
      std::string out(length + 1, '\0');
      size_t copied = 0;
      if (napi_get_value_string_utf8(env, string_value, out.data(), out.size(), &copied) != napi_ok) {
        continue;
      }
      out.resize(copied);
      std::cout << out;
    }
    std::cout << "\n";
  }
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

}  // namespace

napi_status UnodeInstallConsole(napi_env env) {
  if (env == nullptr) {
    return napi_invalid_arg;
  }

  napi_value global = nullptr;
  napi_status status = napi_get_global(env, &global);
  if (status != napi_ok || global == nullptr) {
    return (status == napi_ok) ? napi_generic_failure : status;
  }

  napi_value console_obj = nullptr;
  bool has_console = false;
  status = napi_has_named_property(env, global, "console", &has_console);
  if (status != napi_ok) return status;
  if (has_console) {
    status = napi_get_named_property(env, global, "console", &console_obj);
    if (status != napi_ok || console_obj == nullptr) {
      return (status == napi_ok) ? napi_generic_failure : status;
    }
  } else {
    status = napi_create_object(env, &console_obj);
    if (status != napi_ok || console_obj == nullptr) {
      return (status == napi_ok) ? napi_generic_failure : status;
    }
  }
  napi_value log_fn = nullptr;
  status = napi_create_function(env, "log", NAPI_AUTO_LENGTH, ConsoleLogCallback, nullptr, &log_fn);
  if (status != napi_ok || log_fn == nullptr) {
    return (status == napi_ok) ? napi_generic_failure : status;
  }
  status = napi_set_named_property(env, console_obj, "log", log_fn);
  if (status != napi_ok) {
    return status;
  }
  return napi_set_named_property(env, global, "console", console_obj);
}

int UnodeRunScriptSource(napi_env env, const char* source_text, std::string* error_out) {
  if (error_out != nullptr) {
    error_out->clear();
  }
  if (env == nullptr) {
    if (error_out != nullptr) {
      *error_out = "Invalid environment";
    }
    return 1;
  }
  if (source_text == nullptr || source_text[0] == '\0') {
    if (error_out != nullptr) {
      *error_out = "Empty script source";
    }
    return 1;
  }

  napi_status status = UnodeInstallConsole(env);
  if (status != napi_ok) {
    if (error_out != nullptr) {
      *error_out = "UnodeInstallConsole failed: " + StatusToString(status);
    }
    return 1;
  }

  napi_value script = nullptr;
  status = napi_create_string_utf8(env, source_text, NAPI_AUTO_LENGTH, &script);
  if (status != napi_ok || script == nullptr) {
    if (error_out != nullptr) {
      *error_out = "napi_create_string_utf8 failed: " + StatusToString(status);
    }
    return 1;
  }

  napi_value result = nullptr;
  status = napi_run_script(env, script, &result);
  if (status == napi_ok) {
    return 0;
  }

  const std::string exception_message = GetAndClearPendingException(env);
  if (error_out != nullptr) {
    if (!exception_message.empty()) {
      *error_out = exception_message;
    } else {
      *error_out = "napi_run_script failed: " + StatusToString(status);
    }
  }
  return 1;
}

int UnodeRunScriptFile(napi_env env, const char* script_path, std::string* error_out) {
  if (error_out != nullptr) {
    error_out->clear();
  }
  const std::string source = ReadTextFile(script_path);
  if (source.empty()) {
    if (error_out != nullptr) {
      *error_out = "Failed to read script file";
    }
    return 1;
  }
  return UnodeRunScriptSource(env, source.c_str(), error_out);
}
