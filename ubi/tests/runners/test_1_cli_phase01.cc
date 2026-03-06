#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

#include "test_env.h"
#include "ubi_cli.h"

class Test1CliPhase01 : public FixtureTestBase {};

namespace {

std::string WriteTempScript(const std::string& stem, const std::string& contents) {
  const auto temp_dir = std::filesystem::temp_directory_path();
  const auto unique_name =
      stem + "_" + std::to_string(static_cast<unsigned long long>(std::hash<std::string>{}(contents))) + ".js";
  const auto script_path = temp_dir / unique_name;
  std::ofstream out(script_path);
  out << contents;
  out.close();
  return script_path.string();
}

void RemoveTempScript(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

std::string GetEnvOrEmpty(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr ? std::string(value) : std::string();
}

std::filesystem::path ResolveBuiltBinary(const char* name) {
  namespace fs = std::filesystem;
  const fs::path cwd = fs::current_path();
  const std::vector<fs::path> candidates = {
      cwd / name,
      cwd / "build-ubi-rename" / name,
      cwd / "build-ubi" / name,
      cwd / "build" / name,
      cwd.parent_path() / name,
      cwd.parent_path() / "build-ubi-rename" / name,
      cwd.parent_path() / "build-ubi" / name,
      cwd.parent_path() / "build" / name,
  };
  for (const auto& candidate : candidates) {
    std::error_code ec;
    if (!fs::exists(candidate, ec) || ec) continue;
    if (fs::is_directory(candidate, ec) || ec) continue;
    return fs::absolute(candidate).lexically_normal();
  }
  return {};
}

std::filesystem::path ResolveBuiltUbiBinary() {
  return ResolveBuiltBinary("ubi");
}

std::filesystem::path ResolveBuiltUbienvBinary() {
  return ResolveBuiltBinary("ubienv");
}

std::string ShellSingleQuoted(const std::string& input) {
  std::string out;
  out.reserve(input.size() + 2);
  out.push_back('\'');
  for (char c : input) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out.push_back(c);
    }
  }
  out.push_back('\'');
  return out;
}

std::string JsSingleQuoted(const std::string& input) {
  std::string out;
  out.reserve(input.size() + 2);
  out.push_back('\'');
  for (char c : input) {
    if (c == '\\' || c == '\'') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  out.push_back('\'');
  return out;
}

struct CommandResult {
  int status = -1;
  std::string stdout_output;
  std::string stderr_output;
};

CommandResult RunBuiltBinaryAndCapture(const std::filesystem::path& binary,
                                       const std::vector<std::string>& args,
                                       const std::string& stem) {
  namespace fs = std::filesystem;
  std::string unique_key = binary.string();
  for (const auto& arg : args) {
    unique_key.append("\n");
    unique_key.append(arg);
  }

  const fs::path temp_root =
      fs::temp_directory_path() /
      (stem + "_" + std::to_string(static_cast<unsigned long long>(std::hash<std::string>{}(unique_key))));
  const fs::path stdout_path = temp_root / "stdout.txt";
  const fs::path stderr_path = temp_root / "stderr.txt";
  std::error_code ec;
  fs::remove_all(temp_root, ec);
  fs::create_directories(temp_root, ec);

  std::string cmd = ShellSingleQuoted(binary.string());
  for (const auto& arg : args) {
    cmd.push_back(' ');
    cmd += ShellSingleQuoted(arg);
  }
  cmd += " >" + ShellSingleQuoted(stdout_path.string()) + " 2>" + ShellSingleQuoted(stderr_path.string());

  CommandResult result;
  result.status = std::system(cmd.c_str());

  std::ifstream stdout_in(stdout_path);
  result.stdout_output.assign(std::istreambuf_iterator<char>(stdout_in), std::istreambuf_iterator<char>());
  std::ifstream stderr_in(stderr_path);
  result.stderr_output.assign(std::istreambuf_iterator<char>(stderr_in), std::istreambuf_iterator<char>());

  fs::remove_all(temp_root, ec);
  return result;
}

}  // namespace

TEST_F(Test1CliPhase01, NoArgsWithStdinEofFallsBackToStdinMode) {
#if defined(_WIN32)
  GTEST_SKIP() << "stdin EOF subprocess check is POSIX-only";
#else
  const auto ubi_path = ResolveBuiltUbiBinary();
  ASSERT_FALSE(ubi_path.empty()) << "Failed to resolve built ubi binary";

  const auto temp_root = std::filesystem::temp_directory_path() / "ubi_phase01_cli_no_args";
  const auto stdout_path = temp_root / "stdout.txt";
  const auto stderr_path = temp_root / "stderr.txt";
  std::error_code ec;
  std::filesystem::remove_all(temp_root, ec);
  std::filesystem::create_directories(temp_root, ec);
  ASSERT_FALSE(ec) << "Failed to create temp directory";

  const std::string cmd =
      ShellSingleQuoted(ubi_path.string()) + " </dev/null >" +
      ShellSingleQuoted(stdout_path.string()) + " 2>" +
      ShellSingleQuoted(stderr_path.string());
  const int status = std::system(cmd.c_str());
  ASSERT_NE(status, -1);
  ASSERT_TRUE(WIFEXITED(status)) << "status=" << status;
  EXPECT_EQ(WEXITSTATUS(status), 0);

  std::ifstream stderr_in(stderr_path);
  std::string stderr_output((std::istreambuf_iterator<char>(stderr_in)),
                            std::istreambuf_iterator<char>());
  EXPECT_TRUE(stderr_output.empty()) << "stderr=" << stderr_output;

  std::filesystem::remove_all(temp_root, ec);
#endif
}

TEST_F(Test1CliPhase01, CompatWrappedCommandsBypassCliParsingAndPrefixPath) {
#if defined(_WIN32)
  GTEST_SKIP() << "compat wrapper subprocess check is POSIX-oriented";
#else
  namespace fs = std::filesystem;
  const auto ubi_path = ResolveBuiltUbiBinary();
  ASSERT_FALSE(ubi_path.empty()) << "Failed to resolve built ubi binary";

  const auto temp_root = fs::temp_directory_path() / "ubi_phase01_cli_compat_wrap";
  const auto install_bin_dir = temp_root / "bin";
  const auto compat_dir = temp_root / "bin-compat";
  const auto compat_node = compat_dir / "node";
  const auto stdout_path = temp_root / "stdout.txt";
  const auto stderr_path = temp_root / "stderr.txt";
  std::error_code ec;
  fs::remove_all(temp_root, ec);
  fs::create_directories(install_bin_dir, ec);
  ASSERT_FALSE(ec) << "Failed to create install bin directory";
  fs::create_directories(compat_dir, ec);
  ASSERT_FALSE(ec) << "Failed to create compat directory";

  std::ofstream compat_out(compat_node);
  compat_out
      << "#!/bin/sh\n"
      << "printf 'args=%s\\n' \"$*\"\n"
      << "printf 'path0=%s\\n' \"${PATH%%:*}\"\n";
  compat_out.close();
  ASSERT_TRUE(compat_out.good()) << "Failed to write compat node shim";
  fs::permissions(
      compat_node,
      fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec |
          fs::perms::group_read | fs::perms::group_exec |
          fs::perms::others_read | fs::perms::others_exec,
      fs::perm_options::replace,
      ec);
  ASSERT_FALSE(ec) << "Failed to chmod compat node shim";

  const std::string old_path = GetEnvOrEmpty("PATH");
  const std::string cmd =
      "UBI_EXEC_PATH=" + ShellSingleQuoted((install_bin_dir / "ubi").string()) + " PATH=" +
      ShellSingleQuoted(old_path) + " " + ShellSingleQuoted(ubi_path.string()) +
      " node -p 42 >" + ShellSingleQuoted(stdout_path.string()) +
      " 2>" + ShellSingleQuoted(stderr_path.string());
  const int status = std::system(cmd.c_str());
  ASSERT_NE(status, -1);
  ASSERT_TRUE(WIFEXITED(status)) << "status=" << status;

  std::ifstream stdout_in(stdout_path);
  const std::string stdout_output((std::istreambuf_iterator<char>(stdout_in)),
                                  std::istreambuf_iterator<char>());
  std::ifstream stderr_in(stderr_path);
  const std::string stderr_output((std::istreambuf_iterator<char>(stderr_in)),
                                  std::istreambuf_iterator<char>());
  fs::remove_all(temp_root, ec);

  EXPECT_EQ(WEXITSTATUS(status), 0) << "stderr=" << stderr_output;
  EXPECT_TRUE(stderr_output.empty()) << "stderr=" << stderr_output;
  EXPECT_NE(stdout_output.find("args=-p 42"), std::string::npos) << stdout_output;
  EXPECT_NE(stdout_output.find("path0=" + compat_dir.string()), std::string::npos) << stdout_output;
#endif
}

TEST_F(Test1CliPhase01, CompatWrappedCommandsUseParentBinCompatFromBuildTreeExecPath) {
#if defined(_WIN32)
  GTEST_SKIP() << "compat wrapper subprocess check is POSIX-oriented";
#else
  namespace fs = std::filesystem;
  const auto ubi_path = ResolveBuiltUbiBinary();
  ASSERT_FALSE(ubi_path.empty()) << "Failed to resolve built ubi binary";

  const auto temp_root = fs::temp_directory_path() / "ubi_phase01_cli_compat_build_tree";
  const auto build_dir = temp_root / "build-ubi-rename";
  const auto compat_dir = temp_root / "bin-compat";
  const auto compat_node = compat_dir / "node";
  const auto stdout_path = temp_root / "stdout.txt";
  const auto stderr_path = temp_root / "stderr.txt";
  std::error_code ec;
  fs::remove_all(temp_root, ec);
  fs::create_directories(build_dir, ec);
  ASSERT_FALSE(ec) << "Failed to create build directory";
  fs::create_directories(compat_dir, ec);
  ASSERT_FALSE(ec) << "Failed to create compat directory";

  std::ofstream compat_out(compat_node);
  compat_out
      << "#!/bin/sh\n"
      << "printf 'args=%s\\n' \"$*\"\n"
      << "printf 'path0=%s\\n' \"${PATH%%:*}\"\n";
  compat_out.close();
  ASSERT_TRUE(compat_out.good()) << "Failed to write compat node shim";
  fs::permissions(
      compat_node,
      fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec |
          fs::perms::group_read | fs::perms::group_exec |
          fs::perms::others_read | fs::perms::others_exec,
      fs::perm_options::replace,
      ec);
  ASSERT_FALSE(ec) << "Failed to chmod compat node shim";

  const std::string old_path = GetEnvOrEmpty("PATH");
  const std::string cmd =
      "UBI_EXEC_PATH=" + ShellSingleQuoted((build_dir / "ubi").string()) + " PATH=" +
      ShellSingleQuoted(old_path) + " " + ShellSingleQuoted(ubi_path.string()) +
      " node -p 42 >" + ShellSingleQuoted(stdout_path.string()) +
      " 2>" + ShellSingleQuoted(stderr_path.string());
  const int status = std::system(cmd.c_str());
  ASSERT_NE(status, -1);
  ASSERT_TRUE(WIFEXITED(status)) << "status=" << status;

  std::ifstream stdout_in(stdout_path);
  const std::string stdout_output((std::istreambuf_iterator<char>(stdout_in)),
                                  std::istreambuf_iterator<char>());
  std::ifstream stderr_in(stderr_path);
  const std::string stderr_output((std::istreambuf_iterator<char>(stderr_in)),
                                  std::istreambuf_iterator<char>());
  fs::remove_all(temp_root, ec);

  EXPECT_EQ(WEXITSTATUS(status), 0) << "stderr=" << stderr_output;
  EXPECT_TRUE(stderr_output.empty()) << "stderr=" << stderr_output;
  EXPECT_NE(stdout_output.find("args=-p 42"), std::string::npos) << stdout_output;
  EXPECT_NE(stdout_output.find("path0=" + compat_dir.string()), std::string::npos) << stdout_output;
#endif
}

TEST_F(Test1CliPhase01, UbienvAlwaysPrefixesPathAndBypassesCliParsing) {
#if defined(_WIN32)
  GTEST_SKIP() << "compat wrapper subprocess check is POSIX-oriented";
#else
  namespace fs = std::filesystem;
  const auto ubienv_path = ResolveBuiltUbienvBinary();
  ASSERT_FALSE(ubienv_path.empty()) << "Failed to resolve built ubienv binary";

  const auto temp_root = fs::temp_directory_path() / "ubi_phase01_ubienv_wrap";
  const auto build_dir = temp_root / "build-ubi-rename";
  const auto compat_dir = temp_root / "bin-compat";
  const auto compat_tool = compat_dir / "custom-tool";
  const auto stdout_path = temp_root / "stdout.txt";
  const auto stderr_path = temp_root / "stderr.txt";
  std::error_code ec;
  fs::remove_all(temp_root, ec);
  fs::create_directories(build_dir, ec);
  ASSERT_FALSE(ec) << "Failed to create build directory";
  fs::create_directories(compat_dir, ec);
  ASSERT_FALSE(ec) << "Failed to create compat directory";

  std::ofstream compat_out(compat_tool);
  compat_out
      << "#!/bin/sh\n"
      << "printf 'args=%s\\n' \"$*\"\n"
      << "printf 'path0=%s\\n' \"${PATH%%:*}\"\n";
  compat_out.close();
  ASSERT_TRUE(compat_out.good()) << "Failed to write compat tool shim";
  fs::permissions(
      compat_tool,
      fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec |
          fs::perms::group_read | fs::perms::group_exec |
          fs::perms::others_read | fs::perms::others_exec,
      fs::perm_options::replace,
      ec);
  ASSERT_FALSE(ec) << "Failed to chmod compat tool shim";

  const std::string old_path = GetEnvOrEmpty("PATH");
  const std::string cmd =
      "UBI_EXEC_PATH=" + ShellSingleQuoted((build_dir / "ubienv").string()) + " PATH=" +
      ShellSingleQuoted(old_path) + " " + ShellSingleQuoted(ubienv_path.string()) +
      " custom-tool -p 42 >" + ShellSingleQuoted(stdout_path.string()) +
      " 2>" + ShellSingleQuoted(stderr_path.string());
  const int status = std::system(cmd.c_str());
  ASSERT_NE(status, -1);
  ASSERT_TRUE(WIFEXITED(status)) << "status=" << status;

  std::ifstream stdout_in(stdout_path);
  const std::string stdout_output((std::istreambuf_iterator<char>(stdout_in)),
                                  std::istreambuf_iterator<char>());
  std::ifstream stderr_in(stderr_path);
  const std::string stderr_output((std::istreambuf_iterator<char>(stderr_in)),
                                  std::istreambuf_iterator<char>());
  fs::remove_all(temp_root, ec);

  EXPECT_EQ(WEXITSTATUS(status), 0) << "stderr=" << stderr_output;
  EXPECT_TRUE(stderr_output.empty()) << "stderr=" << stderr_output;
  EXPECT_NE(stdout_output.find("args=-p 42"), std::string::npos) << stdout_output;
  EXPECT_NE(stdout_output.find("path0=" + compat_dir.string()), std::string::npos) << stdout_output;
#endif
}

TEST_F(Test1CliPhase01, ExtraArgsAreForwardedToScriptArgv) {
  const std::string script_path = WriteTempScript(
      "ubi_phase01_cli_extra_args",
      "console.log(process.argv.slice(2).join(','));\n");
  const char* argv[] = {"ubi", script_path.c_str(), "alpha", "beta", "gamma"};

  testing::internal::CaptureStdout();
  std::string error;
  const int exit_code = UbiRunCli(5, argv, &error);
  const std::string stdout_output = testing::internal::GetCapturedStdout();

  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
  EXPECT_NE(stdout_output.find("alpha,beta,gamma"), std::string::npos);

  RemoveTempScript(script_path);
}

TEST_F(Test1CliPhase01, MissingScriptFileReturnsNonZero) {
  const std::string script_path =
      std::string(NAPI_V8_ROOT_PATH) + "/tests/fixtures/this_file_should_not_exist_phase01.js";
  const char* argv[] = {"ubi", script_path.c_str()};
  std::string error;
  const int exit_code = UbiRunCli(2, argv, &error);
  EXPECT_EQ(exit_code, 1);
  const std::string prefix = "Failed to read script file";
  ASSERT_GE(error.size(), prefix.size()) << "error=" << error;
  EXPECT_EQ(error.rfind(prefix, 0), 0u) << "error=" << error;
  if (error.size() > prefix.size()) {
    ASSERT_GE(error.size(), prefix.size() + 2) << "error=" << error;
    EXPECT_EQ(error.substr(prefix.size(), 2), ": ") << "error=" << error;
  }
}

TEST_F(Test1CliPhase01, ValidFixtureScriptReturnsZero) {
  const std::string script_path = std::string(NAPI_V8_ROOT_PATH) + "/tests/fixtures/phase0_hello.js";
  ASSERT_TRUE(std::filesystem::exists(script_path)) << "Missing fixture: " << script_path;
  const char* argv[] = {"ubi", script_path.c_str()};

  testing::internal::CaptureStdout();
  std::string error;
  const int exit_code = UbiRunCli(2, argv, &error);
  const std::string stdout_output = testing::internal::GetCapturedStdout();

  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
  EXPECT_NE(stdout_output.find("hello from ubi"), std::string::npos);
}

TEST_F(Test1CliPhase01, RelativeScriptPathWithoutDotPrefixRunsFromCwd) {
  const auto temp_root = std::filesystem::temp_directory_path() / "ubi_phase01_relative_entry";
  const auto script_dir = temp_root / "examples";
  const auto script_path = script_dir / "relative_entry.js";
  std::error_code ec;
  std::filesystem::remove_all(temp_root, ec);
  std::filesystem::create_directories(script_dir, ec);
  ASSERT_FALSE(ec) << "Failed to create temp test directories";

  std::ofstream out(script_path);
  out << "console.log('relative entry ok');\n";
  out.close();
  ASSERT_TRUE(out.good()) << "Failed to write temp script";

  const auto original_cwd = std::filesystem::current_path();
  std::filesystem::current_path(temp_root, ec);
  ASSERT_FALSE(ec) << "Failed to set cwd to temp root";

  const char* argv[] = {"ubi", "examples/relative_entry.js"};
  testing::internal::CaptureStdout();
  std::string error;
  const int exit_code = UbiRunCli(2, argv, &error);
  const std::string stdout_output = testing::internal::GetCapturedStdout();

  std::filesystem::current_path(original_cwd, ec);
  std::filesystem::remove_all(temp_root, ec);

  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
  EXPECT_NE(stdout_output.find("relative entry ok"), std::string::npos);
}

TEST_F(Test1CliPhase01, RelativeScriptPathFallsBackToUbiSubdirectory) {
  const auto temp_root = std::filesystem::temp_directory_path() / "ubi_phase01_repo_fallback";
  const auto script_dir = temp_root / "ubi" / "examples";
  const auto script_path = script_dir / "fallback_entry.js";
  std::error_code ec;
  std::filesystem::remove_all(temp_root, ec);
  std::filesystem::create_directories(script_dir, ec);
  ASSERT_FALSE(ec) << "Failed to create temp test directories";

  std::ofstream out(script_path);
  out << "console.log('fallback entry ok');\n";
  out.close();
  ASSERT_TRUE(out.good()) << "Failed to write temp script";

  const auto original_cwd = std::filesystem::current_path();
  std::filesystem::current_path(temp_root, ec);
  ASSERT_FALSE(ec) << "Failed to set cwd to temp root";

  const char* argv[] = {"ubi", "examples/fallback_entry.js"};
  testing::internal::CaptureStdout();
  std::string error;
  const int exit_code = UbiRunCli(2, argv, &error);
  const std::string stdout_output = testing::internal::GetCapturedStdout();

  std::filesystem::current_path(original_cwd, ec);
  std::filesystem::remove_all(temp_root, ec);

  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
  EXPECT_NE(stdout_output.find("fallback entry ok"), std::string::npos);
}

TEST_F(Test1CliPhase01, RuntimeThrownErrorReturnsNonZero) {
  const std::string script_path = WriteTempScript("ubi_phase01_cli_throw", "throw new Error('boom from cli');");
  const char* argv[] = {"ubi", script_path.c_str()};
  std::string error;

  const int exit_code = UbiRunCli(2, argv, &error);
  EXPECT_EQ(exit_code, 1);
  EXPECT_NE(error.find("boom from cli"), std::string::npos);

  RemoveTempScript(script_path);
}

TEST_F(Test1CliPhase01, RuntimeSyntaxErrorReturnsNonZero) {
  const std::string script_path = WriteTempScript("ubi_phase01_cli_syntax", "function (");
  const char* argv[] = {"ubi", script_path.c_str()};
  std::string error;

  const int exit_code = UbiRunCli(2, argv, &error);
  EXPECT_EQ(exit_code, 1);
  EXPECT_FALSE(error.empty());

  RemoveTempScript(script_path);
}

TEST_F(Test1CliPhase01, EvalFlagExecutesSource) {
  const char* argv[] = {"ubi", "-e", "console.log('eval-ok')"};
  testing::internal::CaptureStdout();
  std::string error;
  const int exit_code = UbiRunCli(3, argv, &error);
  const std::string stdout_output = testing::internal::GetCapturedStdout();

  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
  EXPECT_NE(stdout_output.find("eval-ok"), std::string::npos);
}

TEST_F(Test1CliPhase01, PrintFlagEvaluatesExpression) {
  const char* argv[] = {"ubi", "-p", "40 + 2"};
  testing::internal::CaptureStdout();
  std::string error;
  const int exit_code = UbiRunCli(3, argv, &error);
  const std::string stdout_output = testing::internal::GetCapturedStdout();

  EXPECT_EQ(exit_code, 0) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
  EXPECT_NE(stdout_output.find("42"), std::string::npos);
}

TEST_F(Test1CliPhase01, FsPromisesReadFileInsideListenCallbackDoesNotHang) {
#if defined(_WIN32)
  GTEST_SKIP() << "listen/readFile subprocess parity check is POSIX-only";
#else
  namespace fs = std::filesystem;
  const auto ubi_path = ResolveBuiltUbiBinary();
  ASSERT_FALSE(ubi_path.empty()) << "Failed to resolve built ubi binary";

  const fs::path file_path =
      fs::temp_directory_path() / "ubi_phase01_listen_readfile_target.txt";
  {
    std::ofstream out(file_path);
    out << "hello-from-readfile";
    ASSERT_TRUE(out.good()) << "Failed to write readFile fixture";
  }

  const std::string script_path = WriteTempScript(
      "ubi_phase01_cli_listen_readfile",
      "const http = require('http');\n"
      "const fs = require('fs/promises');\n"
      "const timer = setTimeout(() => {\n"
      "  console.error('timeout');\n"
      "  process.exit(9);\n"
      "}, 2000);\n"
      "const server = http.createServer((req, res) => res.end('ok'));\n"
      "server.listen(0, async () => {\n"
      "  try {\n"
      "    const text = await fs.readFile(" + JsSingleQuoted(file_path.string()) + ", 'utf8');\n"
      "    console.log('read:' + text);\n"
      "  } catch (err) {\n"
      "    console.error(err && err.stack || err);\n"
      "    process.exitCode = 1;\n"
      "  } finally {\n"
      "    clearTimeout(timer);\n"
      "    server.close();\n"
      "  }\n"
      "});\n");

  const CommandResult result =
      RunBuiltBinaryAndCapture(ubi_path, {script_path}, "ubi_phase01_cli_listen_readfile_run");

  std::error_code ec;
  fs::remove(file_path, ec);
  RemoveTempScript(script_path);

  ASSERT_NE(result.status, -1);
  ASSERT_TRUE(WIFEXITED(result.status)) << "status=" << result.status;
  EXPECT_EQ(WEXITSTATUS(result.status), 0) << "stderr=" << result.stderr_output;
  EXPECT_NE(result.stdout_output.find("read:hello-from-readfile"), std::string::npos) << result.stdout_output;
  EXPECT_EQ(result.stderr_output.find("timeout"), std::string::npos) << result.stderr_output;
#endif
}

TEST_F(Test1CliPhase01, AsyncLocalStoragePromiseContextSurvivesConcurrentAwaits) {
#if defined(_WIN32)
  GTEST_SKIP() << "AsyncLocalStorage subprocess parity check is POSIX-only";
#else
  const auto ubi_path = ResolveBuiltUbiBinary();
  ASSERT_FALSE(ubi_path.empty()) << "Failed to resolve built ubi binary";

  const std::string script_path = WriteTempScript(
      "ubi_phase01_cli_async_local_storage_promises",
      "const { AsyncLocalStorage } = require('async_hooks');\n"
      "const als = new AsyncLocalStorage();\n"
      "(async () => {\n"
      "  const pa = als.run('A', async () => {\n"
      "    await Promise.resolve();\n"
      "    await Promise.resolve();\n"
      "    return ['A', als.getStore()];\n"
      "  });\n"
      "  const pb = als.run('B', async () => {\n"
      "    await Promise.resolve();\n"
      "    await Promise.resolve();\n"
      "    return ['B', als.getStore()];\n"
      "  });\n"
      "  console.log(JSON.stringify(await Promise.all([pa, pb])));\n"
      "})().catch((err) => {\n"
      "  console.error(err && err.stack || err);\n"
      "  process.exitCode = 1;\n"
      "});\n");

  const CommandResult result =
      RunBuiltBinaryAndCapture(ubi_path, {script_path}, "ubi_phase01_cli_async_local_storage_promises_run");

  RemoveTempScript(script_path);

  ASSERT_NE(result.status, -1);
  ASSERT_TRUE(WIFEXITED(result.status)) << "status=" << result.status;
  EXPECT_EQ(WEXITSTATUS(result.status), 0) << "stderr=" << result.stderr_output;
  EXPECT_TRUE(result.stderr_output.empty()) << "stderr=" << result.stderr_output;
  EXPECT_NE(result.stdout_output.find("[[\"A\",\"A\"],[\"B\",\"B\"]]"), std::string::npos)
      << result.stdout_output;
#endif
}

TEST_F(Test1CliPhase01, TextDecoderAcceptsSharedArrayBufferInput) {
#if defined(_WIN32)
  GTEST_SKIP() << "SharedArrayBuffer subprocess parity check is POSIX-only";
#else
  const auto ubi_path = ResolveBuiltUbiBinary();
  ASSERT_FALSE(ubi_path.empty()) << "Failed to resolve built ubi binary";

  const std::string script_path = WriteTempScript(
      "ubi_phase01_cli_textdecoder_shared_arraybuffer",
      "const sab = new SharedArrayBuffer(3);\n"
      "new Uint8Array(sab).set([0x66, 0x6f, 0x6f]);\n"
      "console.log(new TextDecoder().decode(sab));\n");

  const CommandResult result =
      RunBuiltBinaryAndCapture(ubi_path, {script_path}, "ubi_phase01_cli_textdecoder_shared_arraybuffer_run");

  RemoveTempScript(script_path);

  ASSERT_NE(result.status, -1);
  ASSERT_TRUE(WIFEXITED(result.status)) << "status=" << result.status;
  EXPECT_EQ(WEXITSTATUS(result.status), 0) << "stderr=" << result.stderr_output;
  EXPECT_TRUE(result.stderr_output.empty()) << "stderr=" << result.stderr_output;
  EXPECT_NE(result.stdout_output.find("foo"), std::string::npos) << result.stdout_output;
#endif
}

TEST_F(Test1CliPhase01, FsPromisesMkdirRecursiveReturnsPromise) {
#if defined(_WIN32)
  GTEST_SKIP() << "fs.promises.mkdir subprocess parity check is POSIX-only";
#else
  const auto ubi_path = ResolveBuiltUbiBinary();
  ASSERT_FALSE(ubi_path.empty()) << "Failed to resolve built ubi binary";

  const std::string script_path = WriteTempScript(
      "ubi_phase01_cli_fs_promises_mkdir",
      "const fs = require('fs/promises');\n"
      "const path = require('path');\n"
      "const os = require('os');\n"
      "process.on('unhandledRejection', (err) => {\n"
      "  console.error('unhandledRejection', err && err.stack || err);\n"
      "  process.exitCode = 1;\n"
      "});\n"
      "(async () => {\n"
      "  const dir = path.join(os.tmpdir(), 'ubi-fs-promises-mkdir-' + process.pid, 'a', 'b');\n"
      "  const firstCreated = await fs.mkdir(dir, { recursive: true });\n"
      "  console.log('mkdir:' + firstCreated);\n"
      "})().catch((err) => {\n"
      "  console.error(err && err.stack || err);\n"
      "  process.exitCode = 1;\n"
      "});\n");

  const CommandResult result =
      RunBuiltBinaryAndCapture(ubi_path, {script_path}, "ubi_phase01_cli_fs_promises_mkdir_run");

  RemoveTempScript(script_path);

  ASSERT_NE(result.status, -1);
  ASSERT_TRUE(WIFEXITED(result.status)) << "status=" << result.status;
  EXPECT_EQ(WEXITSTATUS(result.status), 0) << "stderr=" << result.stderr_output;
  EXPECT_TRUE(result.stderr_output.empty()) << "stderr=" << result.stderr_output;
  EXPECT_EQ(result.stdout_output.find("mkdir:"), 0U) << result.stdout_output;
#endif
}

TEST_F(Test1CliPhase01, FsPromisesCoreFileOpsReturnPromises) {
#if defined(_WIN32)
  GTEST_SKIP() << "fs.promises core file ops subprocess parity check is POSIX-only";
#else
  const auto ubi_path = ResolveBuiltUbiBinary();
  ASSERT_FALSE(ubi_path.empty()) << "Failed to resolve built ubi binary";

  const std::string script_path = WriteTempScript(
      "ubi_phase01_cli_fs_promises_core_ops",
      "const fs = require('fs/promises');\n"
      "const path = require('path');\n"
      "const os = require('os');\n"
      "process.on('unhandledRejection', (err) => {\n"
      "  console.error('unhandledRejection', err && err.stack || err);\n"
      "  process.exitCode = 1;\n"
      "});\n"
      "(async () => {\n"
      "  const root = path.join(os.tmpdir(), 'ubi-fs-promises-core-' + process.pid);\n"
      "  await fs.mkdir(root, { recursive: true });\n"
      "  const a = path.join(root, 'a.txt');\n"
      "  const b = path.join(root, 'b.txt');\n"
      "  const c = path.join(root, 'c.txt');\n"
      "  const link = path.join(root, 'link.txt');\n"
      "  await fs.writeFile(a, 'alpha');\n"
      "  await fs.rename(a, b);\n"
      "  await fs.copyFile(b, c);\n"
      "  await fs.symlink(c, link);\n"
      "  const linkTarget = await fs.readlink(link, 'utf8');\n"
      "  console.log('ops:' + [path.basename(b), path.basename(c), path.basename(linkTarget)].join(','));\n"
      "})().catch((err) => {\n"
      "  console.error(err && err.stack || err);\n"
      "  process.exitCode = 1;\n"
      "});\n");

  const CommandResult result =
      RunBuiltBinaryAndCapture(ubi_path, {script_path}, "ubi_phase01_cli_fs_promises_core_ops_run");

  RemoveTempScript(script_path);

  ASSERT_NE(result.status, -1);
  ASSERT_TRUE(WIFEXITED(result.status)) << "status=" << result.status;
  EXPECT_EQ(WEXITSTATUS(result.status), 0) << "stderr=" << result.stderr_output;
  EXPECT_TRUE(result.stderr_output.empty()) << "stderr=" << result.stderr_output;
  EXPECT_NE(result.stdout_output.find("ops:b.txt,c.txt,c.txt"), std::string::npos) << result.stdout_output;
#endif
}

TEST_F(Test1CliPhase01, BeforeExitCanScheduleMoreWork) {
  const std::string script_path = WriteTempScript(
      "ubi_phase01_cli_before_exit_loop",
      "const dns = require('dns');\n"
      "let count = 0;\n"
      "process.on('beforeExit', (code) => {\n"
      "  console.log('beforeExit:' + count + ':' + code);\n"
      "  if (count === 0) {\n"
      "    count += 1;\n"
      "    dns.lookup('localhost', () => {\n"
      "      console.log('lookup-fired');\n"
      "      process.exitCode = 3;\n"
      "    });\n"
      "  }\n"
      "});\n"
      "process.on('exit', (code) => console.log('exit:' + code));\n");
  const char* argv[] = {"ubi", script_path.c_str()};

  testing::internal::CaptureStdout();
  std::string error;
  const int exit_code = UbiRunCli(2, argv, &error);
  const std::string stdout_output = testing::internal::GetCapturedStdout();

  EXPECT_EQ(exit_code, 3) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
  EXPECT_NE(stdout_output.find("beforeExit:0:0"), std::string::npos);
  EXPECT_NE(stdout_output.find("lookup-fired"), std::string::npos);
  EXPECT_NE(stdout_output.find("beforeExit:1:3"), std::string::npos);
  EXPECT_NE(stdout_output.find("exit:3"), std::string::npos);

  RemoveTempScript(script_path);
}

TEST_F(Test1CliPhase01, ProcessExitCodeWithoutExplicitProcessExitIsReturned) {
  const std::string script_path = WriteTempScript(
      "ubi_phase01_cli_exit_code_only",
      "process.exitCode = 7;\n"
      "process.on('exit', (code) => console.log('exit:' + code));\n");
  const char* argv[] = {"ubi", script_path.c_str()};

  testing::internal::CaptureStdout();
  std::string error;
  const int exit_code = UbiRunCli(2, argv, &error);
  const std::string stdout_output = testing::internal::GetCapturedStdout();

  EXPECT_EQ(exit_code, 7) << "error=" << error;
  EXPECT_TRUE(error.empty()) << "error=" << error;
  EXPECT_NE(stdout_output.find("exit:7"), std::string::npos);

  RemoveTempScript(script_path);
}

TEST_F(Test1CliPhase01, ExplicitProcessExitDoesNotEmitBeforeExit) {
#if defined(_WIN32)
  GTEST_SKIP() << "explicit process exit CLI parity check is POSIX-only";
#else
  const std::string script_path = WriteTempScript(
      "ubi_phase01_cli_explicit_exit",
      "process.on('beforeExit', () => console.log('beforeExit'));\n"
      "process.on('exit', (code) => console.log('exit:' + code));\n"
      "process.exit(5);\n");
  const auto ubi_path = ResolveBuiltUbiBinary();
  ASSERT_FALSE(ubi_path.empty()) << "Failed to resolve built ubi binary";

  const CommandResult result =
      RunBuiltBinaryAndCapture(ubi_path, {script_path}, "ubi_phase01_cli_explicit_exit_run");

  ASSERT_NE(result.status, -1);
  ASSERT_TRUE(WIFEXITED(result.status)) << "status=" << result.status;
  EXPECT_EQ(WEXITSTATUS(result.status), 5) << "stderr=" << result.stderr_output;
  EXPECT_TRUE(result.stderr_output.empty()) << "stderr=" << result.stderr_output;
  EXPECT_EQ(result.stdout_output.find("beforeExit"), std::string::npos) << result.stdout_output;
  EXPECT_NE(result.stdout_output.find("exit:5"), std::string::npos) << result.stdout_output;

  RemoveTempScript(script_path);
#endif
}

TEST_F(Test1CliPhase01, ExplicitProcessExitInsideAsyncDoesNotBecomeUnhandledRejection) {
#if defined(_WIN32)
  GTEST_SKIP() << "async process exit CLI parity check is POSIX-only";
#else
  const std::string script_path = WriteTempScript(
      "ubi_phase01_cli_async_exit",
      "process.on('unhandledRejection', (err) => {\n"
      "  console.error('unhandledRejection', err && err.code, err && err.__ubiExitCode);\n"
      "});\n"
      "(async () => {\n"
      "  await 0;\n"
      "  process.exit(0);\n"
      "})();\n"
      "setTimeout(() => console.log('timer fired'), 50);\n");
  const auto ubi_path = ResolveBuiltUbiBinary();
  ASSERT_FALSE(ubi_path.empty()) << "Failed to resolve built ubi binary";

  const CommandResult result =
      RunBuiltBinaryAndCapture(ubi_path, {script_path}, "ubi_phase01_cli_async_exit_run");

  ASSERT_NE(result.status, -1);
  ASSERT_TRUE(WIFEXITED(result.status)) << "status=" << result.status;
  EXPECT_EQ(WEXITSTATUS(result.status), 0) << "stderr=" << result.stderr_output;
  EXPECT_EQ(result.stdout_output.find("timer fired"), std::string::npos) << result.stdout_output;
  EXPECT_EQ(result.stderr_output.find("unhandledRejection"), std::string::npos) << result.stderr_output;
  EXPECT_EQ(result.stderr_output.find("ERR_UBI_PROCESS_EXIT"), std::string::npos) << result.stderr_output;

  RemoveTempScript(script_path);
#endif
}

TEST_F(Test1CliPhase01, ProcessExitFromBeforeExitRunsImmediately) {
#if defined(_WIN32)
  GTEST_SKIP() << "beforeExit process exit CLI parity check is POSIX-only";
#else
  const std::string script_path = WriteTempScript(
      "ubi_phase01_cli_before_exit_exit",
      "process.on('beforeExit', () => {\n"
      "  console.log('beforeExit');\n"
      "  setTimeout(() => console.log('timer fired'), 5);\n"
      "  process.exit(0);\n"
      "  console.log('afterExit');\n"
      "});\n");
  const auto ubi_path = ResolveBuiltUbiBinary();
  ASSERT_FALSE(ubi_path.empty()) << "Failed to resolve built ubi binary";

  const CommandResult result =
      RunBuiltBinaryAndCapture(ubi_path, {script_path}, "ubi_phase01_cli_before_exit_exit_run");

  ASSERT_NE(result.status, -1);
  ASSERT_TRUE(WIFEXITED(result.status)) << "status=" << result.status;
  EXPECT_EQ(WEXITSTATUS(result.status), 0) << "stderr=" << result.stderr_output;
  EXPECT_TRUE(result.stderr_output.empty()) << "stderr=" << result.stderr_output;
  EXPECT_NE(result.stdout_output.find("beforeExit"), std::string::npos) << result.stdout_output;
  EXPECT_EQ(result.stdout_output.find("timer fired"), std::string::npos) << result.stdout_output;
  EXPECT_EQ(result.stdout_output.find("afterExit"), std::string::npos) << result.stdout_output;

  RemoveTempScript(script_path);
#endif
}

TEST_F(Test1CliPhase01, ProcessExitCallsMonkeyPatchedReallyExit) {
#if defined(_WIN32)
  GTEST_SKIP() << "process.reallyExit CLI parity check is POSIX-only";
#else
  const std::string script_path = WriteTempScript(
      "ubi_phase01_cli_really_exit_patch",
      "process.reallyExit = function(code) {\n"
      "  console.log('really exited:' + code);\n"
      "};\n"
      "process.exit();\n");
  const auto ubi_path = ResolveBuiltUbiBinary();
  ASSERT_FALSE(ubi_path.empty()) << "Failed to resolve built ubi binary";

  const CommandResult result =
      RunBuiltBinaryAndCapture(ubi_path, {script_path}, "ubi_phase01_cli_really_exit_patch_run");

  ASSERT_NE(result.status, -1);
  ASSERT_TRUE(WIFEXITED(result.status)) << "status=" << result.status;
  EXPECT_EQ(WEXITSTATUS(result.status), 0) << "stderr=" << result.stderr_output;
  EXPECT_TRUE(result.stderr_output.empty()) << "stderr=" << result.stderr_output;
  EXPECT_NE(result.stdout_output.find("really exited:0"), std::string::npos) << result.stdout_output;

  RemoveTempScript(script_path);
#endif
}
