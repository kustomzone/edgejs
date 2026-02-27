#include "unode_cli.h"

#include <filesystem>
#include <string>
#include <vector>

#include "unofficial_napi.h"
#include "unode_runtime.h"

namespace {

constexpr const char kUsage[] = "Usage: unode <script.js>";

std::string ResolveCliScriptPath(const char* script_path) {
  if (script_path == nullptr || script_path[0] == '\0') {
    return "";
  }
  std::filesystem::path direct(script_path);
  if (direct.is_absolute() || std::filesystem::exists(direct)) {
    return direct.string();
  }

  // Allow running `./build/unode examples/foo.js` from repo root.
  const std::filesystem::path repo_fallback = std::filesystem::path("unode") / direct;
  if (std::filesystem::exists(repo_fallback)) {
    return repo_fallback.string();
  }
  return direct.string();
}

}  // namespace

int UnodeRunCliScript(const char* script_path, std::string* error_out) {
  if (error_out != nullptr) {
    error_out->clear();
  }
  if (script_path == nullptr || script_path[0] == '\0') {
    if (error_out != nullptr) {
      *error_out = kUsage;
    }
    return 1;
  }

  napi_env env = nullptr;
  void* env_scope = nullptr;
  const napi_status create_status = unofficial_napi_create_env(8, &env, &env_scope);
  if (create_status != napi_ok || env == nullptr || env_scope == nullptr) {
    if (error_out != nullptr) {
      *error_out = "Failed to initialize runtime environment";
    }
    return 1;
  }

  const std::string resolved_script_path = ResolveCliScriptPath(script_path);
  const int exit_code = UnodeRunScriptFileWithLoop(
      env, resolved_script_path.c_str(), error_out, true);
  const napi_status release_status = unofficial_napi_release_env(env_scope);
  if (release_status != napi_ok) {
    if (error_out != nullptr) {
      *error_out = "Failed to release runtime environment";
    }
    return 1;
  }

  return exit_code;
}

int UnodeRunCli(int argc, const char* const* argv, std::string* error_out) {
  if (error_out != nullptr) {
    error_out->clear();
  }
  if (argv == nullptr || argc < 2) {
    UnodeSetScriptArgv({});
    if (error_out != nullptr) {
      *error_out = kUsage;
    }
    return 1;
  }
  std::vector<std::string> script_argv;
  script_argv.reserve(static_cast<size_t>(argc - 2));
  for (int i = 2; i < argc; ++i) {
    if (argv[i] != nullptr) {
      script_argv.emplace_back(argv[i]);
    }
  }
  UnodeSetScriptArgv(script_argv);
  return UnodeRunCliScript(argv[1], error_out);
}
