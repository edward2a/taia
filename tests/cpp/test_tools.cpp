#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

#include <unistd.h>

#include "tools/tools.hpp"

namespace {

namespace fs = std::filesystem;
using taia::tools::ToolContext;
using taia::tools::ToolRegistry;

int failures = 0;

void check(bool condition, const std::string& what) {
  if (!condition) {
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
  }
}

class TempWorkspace {
 public:
  TempWorkspace() {
    static int counter = 0;
    std::error_code ec;
    path_ = fs::temp_directory_path(ec) /
            ("taia_tools_" + std::to_string(::getpid()) + "_" +
             std::to_string(counter++));
    fs::remove_all(path_, ec);
    fs::create_directories(path_ / "sub", ec);
    std::ofstream(path_ / "alpha.txt") << "hello world\nsecond line\n";
    std::ofstream(path_ / "sub" / "beta.txt") << "hello beta\n";
    std::ofstream(path_ / "AGENTS.md") << "project rules\n";
    std::ofstream(path_ / "binary.bin", std::ios::binary) << '\0' << "data";
  }

  ~TempWorkspace() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }

  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

void test_registry_definitions() {
  const ToolRegistry registry = taia::tools::make_builtin_tools();
  const auto definitions = registry.definitions();
  check(definitions.size() == 5, "five built-in tools");
  check(registry.find("list_files") != nullptr, "list_files registered");
  check(registry.find("search") != nullptr, "search registered");
  check(registry.find("read_file") != nullptr, "read_file registered");
  check(registry.find("write_file") != nullptr, "write_file registered");
  check(registry.find("exec_command") != nullptr, "exec_command registered");
  const auto unknown = registry.execute("nope", "{}", ToolContext{});
  check(!unknown.ok, "unknown tool fails");
  check(unknown.content.find("unknown tool") != std::string::npos,
        "unknown tool message: " + unknown.content);
}

void test_path_boundaries() {
  TempWorkspace workspace;
  const ToolRegistry registry = taia::tools::make_builtin_tools();
  const ToolContext context{workspace.path()};
  auto result =
      registry.execute("read_file", "{\"path\":\"../secret.txt\"}", context);
  check(!result.ok, "parent escape rejected");
  check(result.content.find("escapes workspace") != std::string::npos,
        "parent escape message: " + result.content);
  result = registry.execute("read_file", "{\"path\":\"/etc/passwd\"}", context);
  check(!result.ok, "absolute path rejected");
  check(result.content.find("absolute") != std::string::npos,
        "absolute path message: " + result.content);

  const fs::path outside =
      workspace.path().parent_path() /
      (workspace.path().filename().string() + "_outside");
  std::error_code ec;
  fs::remove_all(outside, ec);
  fs::create_directories(outside, ec);
  std::ofstream(outside / "secret.txt") << "secret\n";
  fs::create_symlink(outside / "secret.txt", workspace.path() / "link.txt", ec);
  result = registry.execute("read_file", "{\"path\":\"link.txt\"}", context);
  check(!result.ok, "symlink escape rejected");
  fs::remove_all(outside, ec);
}

void test_read_file() {
  TempWorkspace workspace;
  const ToolRegistry registry = taia::tools::make_builtin_tools();
  const ToolContext context{workspace.path()};
  auto result = registry.execute(
      "read_file", "{\"path\":\"alpha.txt\",\"max_lines\":1}", context);
  check(result.ok, "read succeeds: " + result.content);
  check(result.content == "1: hello world\n", "read first line");
  result = registry.execute("read_file",
                            "{\"path\":\"alpha.txt\",\"start_line\":2}",
                            context);
  check(result.content == "2: second line\n", "read second line");
  result = registry.execute("read_file", "{\"path\":\"binary.bin\"}", context);
  check(!result.ok, "binary read rejected");
  check(result.content.find("binary") != std::string::npos,
        "binary message: " + result.content);
  result = registry.execute("read_file", "{\"path\":\"missing.txt\"}", context);
  check(!result.ok, "missing file rejected");
}

void test_list_files() {
  TempWorkspace workspace;
  const ToolRegistry registry = taia::tools::make_builtin_tools();
  const ToolContext context{workspace.path()};
  auto result = registry.execute("list_files", "{}", context);
  check(result.ok, "list succeeds: " + result.content);
  check(result.content.find("alpha.txt") != std::string::npos,
        "list includes file");
  check(result.content.find("sub/") != std::string::npos,
        "list includes directory");
  result = registry.execute("list_files", "{\"recursive\":true}", context);
  check(result.content.find("sub/beta.txt") != std::string::npos,
        "recursive list includes nested file");
  result = registry.execute("list_files", "{\"max_entries\":1}", context);
  check(result.content.find("[truncated]") != std::string::npos,
        "list truncates");
}

void test_search() {
  TempWorkspace workspace;
  const ToolRegistry registry = taia::tools::make_builtin_tools();
  const ToolContext context{workspace.path()};
  auto result = registry.execute("search", "{\"query\":\"hello\"}", context);
  check(result.ok, "search succeeds: " + result.content);
  check(result.content.find("alpha.txt:1: hello world") != std::string::npos,
        "search finds first file");
  check(result.content.find("sub/beta.txt:1: hello beta") != std::string::npos,
        "search finds nested file");
  check(result.content.find("binary.bin") == std::string::npos,
        "search skips binary files");
  result = registry.execute(
      "search", "{\"query\":\"HELLO\",\"case_sensitive\":true}", context);
  check(result.content == "(no matches)", "search is case sensitive");
  result = registry.execute(
      "search", "{\"query\":\"HELLO\",\"case_sensitive\":false}", context);
  check(result.content.find("alpha.txt") != std::string::npos,
        "search is case insensitive");
  result = registry.execute(
      "search", "{\"query\":\"hello\",\"max_results\":1}", context);
  check(result.content.find("[truncated]") != std::string::npos,
        "search truncates");
}

void test_agents_md() {
  TempWorkspace workspace;
  const std::string content = taia::tools::load_agents_md(workspace.path());
  check(content == "project rules\n", "AGENTS.md loaded");
  const std::string missing =
      taia::tools::load_agents_md(workspace.path() / "sub");
  check(missing.empty(), "missing AGENTS.md is empty");
  const std::string truncated =
      taia::tools::load_agents_md(workspace.path(), 5);
  check(truncated.find("[AGENTS.md truncated]") != std::string::npos,
        "AGENTS.md truncates");

  const fs::path outside =
      workspace.path().parent_path() /
      (workspace.path().filename().string() + "_outside_agents");
  std::error_code ec;
  fs::remove_all(outside, ec);
  fs::create_directories(outside, ec);
  std::ofstream(outside / "AGENTS.md") << "outside rules\n";
  fs::remove(workspace.path() / "AGENTS.md", ec);
  fs::create_symlink(outside / "AGENTS.md", workspace.path() / "AGENTS.md", ec);
  check(taia::tools::load_agents_md(workspace.path()).empty(),
        "AGENTS.md symlink escape rejected");
  fs::remove_all(outside, ec);
}

void test_write_file() {
  TempWorkspace workspace;
  const ToolRegistry registry = taia::tools::make_builtin_tools();
  std::string preview;
  bool approve = true;
  ToolContext context{workspace.path()};
  context.approve = [&](const std::string& text) {
    preview = text;
    return approve;
  };

  auto result = registry.execute(
      "write_file", "{\"path\":\"new.txt\",\"content\":\"hello\\n\"}",
      context);
  check(result.ok, "write succeeds: " + result.content);
  check(preview.find("(new file)") != std::string::npos, "new file preview");
  check(preview.find("+hello") != std::string::npos,
        "write preview shows content: " + preview);
  std::ifstream written(workspace.path() / "new.txt");
  const std::string content((std::istreambuf_iterator<char>(written)),
                            std::istreambuf_iterator<char>());
  check(content == "hello\n", "written content");

  approve = false;
  result = registry.execute(
      "write_file", "{\"path\":\"rejected.txt\",\"content\":\"x\"}", context);
  check(!result.ok, "rejected write fails");
  check(result.content.find("rejected") != std::string::npos,
        "rejected write message: " + result.content);
  check(!fs::exists(workspace.path() / "rejected.txt"),
        "rejected write did not create file");

  context.approve = [&](const std::string&) {
    std::ofstream(workspace.path() / "alpha.txt") << "changed\n";
    return true;
  };
  result = registry.execute(
      "write_file", "{\"path\":\"alpha.txt\",\"content\":\"new\\n\"}", context);
  check(!result.ok, "changed file write fails");
  check(result.content.find("changed since approval") != std::string::npos,
        "changed file message: " + result.content);
  std::ifstream changed(workspace.path() / "alpha.txt");
  const std::string changed_content((std::istreambuf_iterator<char>(changed)),
                                    std::istreambuf_iterator<char>());
  check(changed_content == "changed\n", "changed file preserved");

  result = registry.execute(
      "write_file", "{\"path\":\"../escape.txt\",\"content\":\"x\"}", context);
  check(!result.ok, "write escape rejected");
  result = registry.execute(
      "write_file", "{\"path\":\"nope/x.txt\",\"content\":\"x\"}", context);
  check(!result.ok, "missing parent rejected");
}

void test_exec_command() {
  TempWorkspace workspace;
  const ToolRegistry registry = taia::tools::make_builtin_tools();
  std::string preview;
  bool approve = true;
  ToolContext context{workspace.path()};
  context.approve = [&](const std::string& text) {
    preview = text;
    return approve;
  };

  auto result =
      registry.execute("exec_command", "{\"command\":\"printf hi\"}", context);
  check(result.ok, "command succeeds: " + result.content);
  check(result.content.find("hi") != std::string::npos,
        "command output: " + result.content);
  check(preview.find("cwd:") != std::string::npos,
        "command preview shows cwd: " + preview);
  check(preview.find("$ printf hi") != std::string::npos,
        "command preview shows command: " + preview);

  result = registry.execute("exec_command", "{\"command\":\"pwd\"}", context);
  check(result.content.find(workspace.path().string()) != std::string::npos,
        "command runs in workspace: " + result.content);

  approve = false;
  result = registry.execute(
      "exec_command", "{\"command\":\"touch rejected.txt\"}", context);
  check(!result.ok, "rejected command fails");
  check(!fs::exists(workspace.path() / "rejected.txt"),
        "rejected command did not run");

  approve = true;
  result = registry.execute(
      "exec_command", "{\"command\":\"sleep 2\",\"timeout_ms\":100}", context);
  check(!result.ok, "command times out");
  check(result.content.find("timed out") != std::string::npos,
        "timeout message: " + result.content);
}

void test_output_cap() {
  TempWorkspace workspace;
  const ToolRegistry registry = taia::tools::make_builtin_tools();
  ToolContext context{workspace.path()};
  context.max_output_bytes = 5;
  const auto result =
      registry.execute("read_file", "{\"path\":\"alpha.txt\"}", context);
  check(result.content.size() <= 5 + std::string("\n[output truncated]").size(),
        "output capped");
  check(result.content.find("[output truncated]") != std::string::npos,
        "output cap marker");
}

}  // namespace

int main() {
  test_registry_definitions();
  test_path_boundaries();
  test_read_file();
  test_list_files();
  test_search();
  test_agents_md();
  test_write_file();
  test_exec_command();
  test_output_cap();
  if (failures == 0) {
    std::cout << "all tool tests passed\n";
    return 0;
  }
  std::cerr << failures << " tool test(s) failed\n";
  return 1;
}
