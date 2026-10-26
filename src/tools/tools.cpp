#include "tools/tools.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <poll.h>
#include <sstream>
#include <system_error>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

#include <nlohmann/json.hpp>

namespace taia::tools {

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr std::size_t kMaxSearchFileBytes = 1024 * 1024;
constexpr std::size_t kMaxReadFileBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaxSearchFiles = 5000;
constexpr std::size_t kMaxLineChars = 2000;
constexpr std::size_t kMaxWriteBytes = 128 * 1024;
constexpr std::size_t kMaxPreviewBytes = 160 * 1024;

ToolResult failure(const std::string& message) { return {false, message}; }

bool cancelled(const ToolContext& context) {
  return context.cancel != nullptr && context.cancel->load();
}

bool within_root(const fs::path& root, const fs::path& candidate) {
  auto root_it = root.begin();
  auto candidate_it = candidate.begin();
  for (; root_it != root.end(); ++root_it, ++candidate_it) {
    if (candidate_it == candidate.end() || *candidate_it != *root_it) {
      return false;
    }
  }
  return true;
}

bool resolve_path(const ToolContext& context, const std::string& requested,
                  fs::path& out, std::string& error) {
  if (requested.empty()) {
    error = "path is required";
    return false;
  }
  const fs::path raw(requested);
  if (raw.is_absolute()) {
    error = "absolute paths are not allowed";
    return false;
  }
  std::error_code ec;
  const fs::path root = fs::weakly_canonical(context.workspace, ec);
  if (ec) {
    error = "cannot resolve workspace: " + ec.message();
    return false;
  }
  const fs::path candidate = fs::weakly_canonical(root / raw, ec);
  if (ec) {
    error = "cannot resolve path: " + ec.message();
    return false;
  }
  if (!within_root(root, candidate)) {
    error = "path escapes workspace";
    return false;
  }
  out = candidate;
  return true;
}

std::string label_for(const fs::path& root, const fs::path& path) {
  const fs::path relative = path.lexically_relative(root);
  const std::string text = relative.generic_string();
  return text.empty() ? "." : text;
}

std::string lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return text;
}

std::size_t utf8_boundary(const std::string& text, std::size_t limit) {
  std::size_t cut = limit;
  while (cut > 0 &&
         (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
    --cut;
  }
  return cut;
}

std::string truncate_line(const std::string& line) {
  if (line.size() <= kMaxLineChars) return line;
  return line.substr(0, utf8_boundary(line, kMaxLineChars)) + "...";
}

bool get_string(const json& args, const char* key, bool required,
                std::string& out, std::string& error) {
  if (!args.contains(key)) {
    if (required) {
      error = std::string(key) + " is required";
      return false;
    }
    return true;
  }
  if (!args[key].is_string()) {
    error = std::string(key) + " must be a string";
    return false;
  }
  out = args[key].get<std::string>();
  return true;
}

bool get_int(const json& args, const char* key, bool required, int& out,
             std::string& error) {
  if (!args.contains(key)) {
    if (required) {
      error = std::string(key) + " is required";
      return false;
    }
    return true;
  }
  if (!args[key].is_number_integer()) {
    error = std::string(key) + " must be an integer";
    return false;
  }
  out = args[key].get<int>();
  return true;
}

bool get_bool(const json& args, const char* key, bool required, bool& out,
              std::string& error) {
  if (!args.contains(key)) {
    if (required) {
      error = std::string(key) + " is required";
      return false;
    }
    return true;
  }
  if (!args[key].is_boolean()) {
    error = std::string(key) + " must be a boolean";
    return false;
  }
  out = args[key].get<bool>();
  return true;
}

bool parse_args(const std::string& text, json& args, std::string& error) {
  if (text.empty()) {
    args = json::object();
    return true;
  }
  try {
    args = json::parse(text);
  } catch (const std::exception& exc) {
    error = std::string("invalid arguments: ") + exc.what();
    return false;
  }
  if (!args.is_object()) {
    error = "arguments must be a JSON object";
    return false;
  }
  return true;
}

class ListFilesTool : public Tool {
 public:
  std::string name() const override { return "list_files"; }

  std::string description() const override {
    return "List files and directories under a workspace-relative path.";
  }

  std::string parameters() const override {
    return R"json({"type":"object","properties":{"path":{"type":"string"},"recursive":{"type":"boolean"},"max_entries":{"type":"integer","minimum":1,"maximum":1000}},"required":[]})json";
  }

  ToolResult execute(const std::string& arguments,
                     const ToolContext& context) const override {
    json args;
    std::string error;
    if (!parse_args(arguments, args, error)) return failure(error);
    std::string path = ".";
    bool recursive = false;
    int max_entries = 200;
    if (!get_string(args, "path", false, path, error)) return failure(error);
    if (!get_bool(args, "recursive", false, recursive, error)) {
      return failure(error);
    }
    if (!get_int(args, "max_entries", false, max_entries, error)) {
      return failure(error);
    }
    if (max_entries < 1 || max_entries > 1000) {
      return failure("max_entries must be between 1 and 1000");
    }
    fs::path target;
    if (!resolve_path(context, path, target, error)) return failure(error);
    std::error_code ec;
    if (!fs::exists(target, ec)) return failure("path does not exist");
    if (!fs::is_directory(target, ec)) {
      return failure("path is not a directory");
    }

    std::vector<std::string> entries;
    bool truncated = false;
    auto add_entry = [&](const fs::directory_entry& entry) {
      if (cancelled(context)) return false;
      std::error_code type_ec;
      const bool is_dir = entry.is_directory(type_ec);
      std::string text = label_for(context.workspace, entry.path());
      if (is_dir) text += "/";
      entries.push_back(std::move(text));
      if (static_cast<int>(entries.size()) >= max_entries) {
        truncated = true;
        return false;
      }
      return true;
    };

    const auto options = fs::directory_options::skip_permission_denied;
    if (recursive) {
      auto it = fs::recursive_directory_iterator(target, options);
      while (it != fs::recursive_directory_iterator()) {
        std::error_code type_ec;
        if (it->is_directory(type_ec) && it->path().filename() == ".git") {
          it.disable_recursion_pending();
          ++it;
          continue;
        }
        if (!add_entry(*it)) break;
        ++it;
      }
    } else {
      for (const auto& entry : fs::directory_iterator(target, options)) {
        if (!add_entry(entry)) break;
      }
    }
    if (cancelled(context)) return failure("cancelled");
    std::sort(entries.begin(), entries.end());
    std::string output;
    for (const std::string& entry : entries) {
      output += entry;
      output += "\n";
    }
    if (truncated) output += "[truncated]\n";
    return {true, output};
  }
};

class SearchTool : public Tool {
 public:
  std::string name() const override { return "search"; }

  std::string description() const override {
    return "Search text files in the workspace for a substring.";
  }

  std::string parameters() const override {
    return R"json({"type":"object","properties":{"query":{"type":"string"},"path":{"type":"string"},"max_results":{"type":"integer","minimum":1,"maximum":500},"case_sensitive":{"type":"boolean"}},"required":["query"]})json";
  }

  ToolResult execute(const std::string& arguments,
                     const ToolContext& context) const override {
    json args;
    std::string error;
    if (!parse_args(arguments, args, error)) return failure(error);
    std::string query;
    std::string path = ".";
    int max_results = 100;
    bool case_sensitive = true;
    if (!get_string(args, "query", true, query, error)) return failure(error);
    if (!get_string(args, "path", false, path, error)) return failure(error);
    if (!get_int(args, "max_results", false, max_results, error)) {
      return failure(error);
    }
    if (!get_bool(args, "case_sensitive", false, case_sensitive, error)) {
      return failure(error);
    }
    if (query.empty()) return failure("query must not be empty");
    if (max_results < 1 || max_results > 500) {
      return failure("max_results must be between 1 and 500");
    }
    fs::path target;
    if (!resolve_path(context, path, target, error)) return failure(error);
    std::error_code ec;
    if (!fs::exists(target, ec)) return failure("path does not exist");

    std::vector<fs::path> files;
    if (fs::is_regular_file(target, ec)) {
      files.push_back(target);
    } else if (fs::is_directory(target, ec)) {
      const auto options = fs::directory_options::skip_permission_denied;
      auto it = fs::recursive_directory_iterator(target, options);
      while (it != fs::recursive_directory_iterator()) {
        if (cancelled(context)) return failure("cancelled");
        std::error_code type_ec;
        if (it->is_directory(type_ec)) {
          if (it->path().filename() == ".git") it.disable_recursion_pending();
          ++it;
          continue;
        }
        if (it->is_regular_file(type_ec) && !it->is_symlink(type_ec)) {
          files.push_back(it->path());
          if (files.size() >= kMaxSearchFiles) break;
        }
        ++it;
      }
    } else {
      return failure("path is not a file or directory");
    }

    std::sort(files.begin(), files.end());
    const std::string needle = case_sensitive ? query : lower(query);
    std::string output;
    int matches = 0;
    bool truncated = false;
    for (const fs::path& file : files) {
      if (cancelled(context)) return failure("cancelled");
      std::error_code size_ec;
      const auto size = fs::file_size(file, size_ec);
      if (size_ec || size > kMaxSearchFileBytes) continue;
      std::ifstream input(file, std::ios::binary);
      if (!input) continue;
      const std::string data((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
      if (data.find('\0') != std::string::npos) continue;
      const std::string label = label_for(context.workspace, file);
      std::istringstream lines(data);
      std::string line;
      int number = 0;
      while (std::getline(lines, line)) {
        ++number;
        if (cancelled(context)) return failure("cancelled");
        const std::string haystack = case_sensitive ? line : lower(line);
        if (haystack.find(needle) == std::string::npos) continue;
        output += label + ":" + std::to_string(number) + ": " +
                  truncate_line(line) + "\n";
        ++matches;
        if (matches >= max_results) {
          truncated = true;
          break;
        }
      }
      if (truncated) break;
    }
    if (output.empty()) return {true, "(no matches)"};
    if (truncated) output += "[truncated]\n";
    return {true, output};
  }
};

class ReadFileTool : public Tool {
 public:
  std::string name() const override { return "read_file"; }

  std::string description() const override {
    return "Read a text file from the workspace with line numbers.";
  }

  std::string parameters() const override {
    return R"json({"type":"object","properties":{"path":{"type":"string"},"start_line":{"type":"integer","minimum":1},"max_lines":{"type":"integer","minimum":1,"maximum":2000}},"required":["path"]})json";
  }

  ToolResult execute(const std::string& arguments,
                     const ToolContext& context) const override {
    json args;
    std::string error;
    if (!parse_args(arguments, args, error)) return failure(error);
    std::string path;
    int start_line = 1;
    int max_lines = 400;
    if (!get_string(args, "path", true, path, error)) return failure(error);
    if (!get_int(args, "start_line", false, start_line, error)) {
      return failure(error);
    }
    if (!get_int(args, "max_lines", false, max_lines, error)) {
      return failure(error);
    }
    if (start_line < 1) return failure("start_line must be at least 1");
    if (max_lines < 1 || max_lines > 2000) {
      return failure("max_lines must be between 1 and 2000");
    }
    fs::path target;
    if (!resolve_path(context, path, target, error)) return failure(error);
    std::error_code ec;
    if (!fs::exists(target, ec)) return failure("path does not exist");
    if (!fs::is_regular_file(target, ec)) {
      return failure("path is not a regular file");
    }
    const auto size = fs::file_size(target, ec);
    if (!ec && size > kMaxReadFileBytes) {
      return failure("file is too large");
    }
    std::ifstream input(target, std::ios::binary);
    if (!input) return failure("cannot open file");

    std::string output;
    std::string line;
    int number = 0;
    int emitted = 0;
    while (std::getline(input, line)) {
      ++number;
      if (cancelled(context)) return failure("cancelled");
      if (number < start_line) continue;
      if (line.find('\0') != std::string::npos) {
        return failure("binary file");
      }
      output += std::to_string(number) + ": " + truncate_line(line) + "\n";
      ++emitted;
      if (emitted >= max_lines) break;
    }
    if (output.empty()) return {true, "(no content)"};
    return {true, output};
  }
};

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    if (end == std::string::npos) {
      lines.push_back(text.substr(start));
      break;
    }
    std::string line = text.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(std::move(line));
    start = end + 1;
  }
  return lines;
}

std::string diff_preview(const std::string& old_text,
                         const std::string& new_text) {
  const std::vector<std::string> old_lines = split_lines(old_text);
  const std::vector<std::string> new_lines = split_lines(new_text);
  std::size_t prefix = 0;
  while (prefix < old_lines.size() && prefix < new_lines.size() &&
         old_lines[prefix] == new_lines[prefix]) {
    ++prefix;
  }
  std::size_t suffix = 0;
  while (suffix < old_lines.size() - prefix &&
         suffix < new_lines.size() - prefix &&
         old_lines[old_lines.size() - 1 - suffix] ==
             new_lines[new_lines.size() - 1 - suffix]) {
    ++suffix;
  }
  std::string preview;
  for (std::size_t i = prefix; i < old_lines.size() - suffix; ++i) {
    preview += "-" + old_lines[i] + "\n";
  }
  for (std::size_t i = prefix; i < new_lines.size() - suffix; ++i) {
    preview += "+" + new_lines[i] + "\n";
  }
  return preview;
}

class WriteFileTool : public Tool {
 public:
  std::string name() const override { return "write_file"; }

  std::string description() const override {
    return "Write a text file in the workspace. Requires approval.";
  }

  std::string parameters() const override {
    return R"json({"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"}},"required":["path","content"]})json";
  }

  ToolResult execute(const std::string& arguments,
                     const ToolContext& context) const override {
    json args;
    std::string error;
    if (!parse_args(arguments, args, error)) return failure(error);
    std::string path;
    std::string content;
    if (!get_string(args, "path", true, path, error)) return failure(error);
    if (!get_string(args, "content", true, content, error)) {
      return failure(error);
    }
    if (content.size() > kMaxWriteBytes) {
      return failure("content is too large");
    }
    fs::path target;
    if (!resolve_path(context, path, target, error)) return failure(error);
    std::error_code ec;
    const bool existed = fs::exists(target, ec);
    std::string old_content;
    if (existed) {
      if (!fs::is_regular_file(target, ec)) {
        return failure("path is not a regular file");
      }
      const auto size = fs::file_size(target, ec);
      if (ec || size > kMaxWriteBytes) return failure("file is too large");
      std::ifstream input(target, std::ios::binary);
      if (!input) return failure("cannot read existing file");
      old_content.assign((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
      if (old_content.find('\0') != std::string::npos) {
        return failure("binary file");
      }
    } else {
      const fs::path parent = target.parent_path();
      if (!fs::is_directory(parent, ec)) {
        return failure("parent directory does not exist");
      }
    }

    const std::string label = label_for(context.workspace, target);
    std::string preview = "write_file " + label + "\n";
    if (!existed) preview += "(new file)\n";
    preview += "--- current\n+++ new\n";
    preview += diff_preview(old_content, content);
    if (preview.size() > kMaxPreviewBytes) {
      return failure("preview is too large");
    }
    if (!context.approve || !context.approve(preview)) {
      return failure("rejected by user");
    }
    if (cancelled(context)) return failure("cancelled");

    std::error_code rec;
    const bool now_exists = fs::exists(target, rec);
    std::string current;
    if (now_exists) {
      if (!fs::is_regular_file(target, rec)) {
        return failure("file changed since approval; not written");
      }
      const auto size = fs::file_size(target, rec);
      if (rec || size > kMaxWriteBytes) {
        return failure("file changed since approval; not written");
      }
      std::ifstream input(target, std::ios::binary);
      if (!input) {
        return failure("file changed since approval; not written");
      }
      current.assign((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
    }
    if (now_exists != existed || current != old_content) {
      return failure("file changed since approval; not written");
    }

    std::string temp_path;
    int temp_fd = -1;
    for (int attempt = 0; attempt < 10 && temp_fd < 0; ++attempt) {
      const std::string temp_template =
          (target.parent_path() /
           (".taia_tmp_" + target.filename().string() + "_XXXXXX"))
              .string();
      std::vector<char> temp_buffer(temp_template.begin(),
                                    temp_template.end());
      temp_buffer.push_back('\0');
      const int probe = ::mkstemp(temp_buffer.data());
      if (probe < 0) return failure("cannot write file");
      const std::string candidate(temp_buffer.data());
      ::close(probe);
      ::unlink(candidate.c_str());
      temp_fd = ::open(candidate.c_str(),
                       O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0666);
      if (temp_fd >= 0) {
        temp_path = candidate;
      } else if (errno != EEXIST) {
        return failure("cannot write file");
      }
    }
    if (temp_fd < 0) return failure("cannot write file");
    struct stat existing {};
    if (existed && ::stat(target.c_str(), &existing) == 0) {
      ::fchmod(temp_fd, existing.st_mode & 0777);
    }
    std::size_t written = 0;
    bool write_ok = true;
    while (written < content.size()) {
      const ssize_t count = ::write(temp_fd, content.data() + written,
                                    content.size() - written);
      if (count < 0) {
        if (errno == EINTR) continue;
        write_ok = false;
        break;
      }
      written += static_cast<std::size_t>(count);
    }
    if (::close(temp_fd) != 0) write_ok = false;
    if (!write_ok) {
      ::unlink(temp_path.c_str());
      return failure("cannot write file");
    }
    if (::rename(temp_path.c_str(), target.c_str()) != 0) {
      const std::string message = std::strerror(errno);
      ::unlink(temp_path.c_str());
      return failure("cannot replace file: " + message);
    }
    return {true, "wrote " + std::to_string(content.size()) + " bytes to " +
                      label};
  }
};

class ExecCommandTool : public Tool {
 public:
  std::string name() const override { return "exec_command"; }

  std::string description() const override {
    return "Run a Linux/bash command in the workspace. Requires approval.";
  }

  std::string parameters() const override {
    return R"json({"type":"object","properties":{"command":{"type":"string"},"timeout_ms":{"type":"integer","minimum":100,"maximum":60000}},"required":["command"]})json";
  }

  ToolResult execute(const std::string& arguments,
                     const ToolContext& context) const override {
    json args;
    std::string error;
    if (!parse_args(arguments, args, error)) return failure(error);
    std::string command;
    int timeout_ms = 10000;
    if (!get_string(args, "command", true, command, error)) {
      return failure(error);
    }
    if (!get_int(args, "timeout_ms", false, timeout_ms, error)) {
      return failure(error);
    }
    if (command.empty()) return failure("command must not be empty");
    if (command.size() > 8192) return failure("command is too long");
    if (timeout_ms < 100 || timeout_ms > 60000) {
      return failure("timeout_ms must be between 100 and 60000");
    }
    const std::string preview = "exec_command\ncwd: " +
                                context.workspace.string() + "\n$ " + command +
                                "\n";
    if (!context.approve || !context.approve(preview)) {
      return failure("rejected by user");
    }
    if (cancelled(context)) return failure("cancelled");
    return run_command(command, timeout_ms, context);
  }

 private:
  ToolResult run_command(const std::string& command, int timeout_ms,
                         const ToolContext& context) const {
    int pipe_fds[2];
    if (::pipe(pipe_fds) != 0) return failure("cannot create pipe");
    const pid_t pid = ::fork();
    if (pid < 0) {
      ::close(pipe_fds[0]);
      ::close(pipe_fds[1]);
      return failure("cannot fork");
    }
    if (pid == 0) {
      ::setpgid(0, 0);
      ::close(pipe_fds[0]);
      ::dup2(pipe_fds[1], STDOUT_FILENO);
      ::dup2(pipe_fds[1], STDERR_FILENO);
      ::close(pipe_fds[1]);
      ::unsetenv("TAIA_API_KEY");
      if (::chdir(context.workspace.c_str()) != 0) ::_exit(126);
      ::execl("/bin/bash", "bash", "-lc", command.c_str(),
              static_cast<char*>(nullptr));
      ::_exit(127);
    }
    ::setpgid(pid, pid);
    ::close(pipe_fds[1]);
    const int flags = ::fcntl(pipe_fds[0], F_GETFL, 0);
    if (flags >= 0) ::fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK);

    std::string output;
    bool truncated = false;
    bool timed_out = false;
    bool killed = false;
    int status = 0;
    bool exited = false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);

    auto append_output = [&](const char* data, std::size_t size) {
      if (output.size() >= context.max_output_bytes) {
        truncated = true;
        return;
      }
      const std::size_t room = context.max_output_bytes - output.size();
      const std::size_t take = std::min(room, size);
      output.append(data, take);
      if (take < size) truncated = true;
    };

    auto kill_child = [&] {
      if (!killed) {
        ::kill(-pid, SIGKILL);
        ::kill(pid, SIGKILL);
        killed = true;
      }
    };

    while (!exited) {
      if (cancelled(context)) {
        kill_child();
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        ::close(pipe_fds[0]);
        return failure("cancelled");
      }
      if (!timed_out && std::chrono::steady_clock::now() >= deadline) {
        timed_out = true;
        kill_child();
      }
      pollfd descriptor{pipe_fds[0], POLLIN, 0};
      const int ready = ::poll(&descriptor, 1, 100);
      if (ready > 0) {
        char buffer[4096];
        const ssize_t count = ::read(pipe_fds[0], buffer, sizeof(buffer));
        if (count > 0) {
          append_output(buffer, static_cast<std::size_t>(count));
        }
      } else if (ready < 0 && errno != EINTR) {
        break;
      }
      siginfo_t info {};
      const int wait_rc =
          ::waitid(P_PID, static_cast<id_t>(pid), &info,
                   WEXITED | WNOHANG | WNOWAIT);
      if (wait_rc == 0 && info.si_pid == pid) {
        kill_child();
        exited = true;
      } else if (wait_rc < 0 && errno == ECHILD) {
        ::close(pipe_fds[0]);
        return failure("lost child process");
      }
    }

    if (!exited) {
      kill_child();
      while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
    } else {
      while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
    }
    while (true) {
      char buffer[4096];
      const ssize_t count = ::read(pipe_fds[0], buffer, sizeof(buffer));
      if (count > 0) {
        append_output(buffer, static_cast<std::size_t>(count));
        continue;
      }
      if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && !exited) {
        continue;
      }
      break;
    }
    ::close(pipe_fds[0]);

    if (timed_out) {
      return {false, "command timed out after " + std::to_string(timeout_ms) +
                         " ms\n" + output};
    }
    if (WIFSIGNALED(status)) {
      return {false, "command terminated by signal " +
                         std::to_string(WTERMSIG(status)) + "\n" + output};
    }
    const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    std::string text = "exit code " + std::to_string(exit_code) + "\n" + output;
    if (truncated) text += "\n[output truncated]";
    return {exit_code == 0, std::move(text)};
  }
};

}  // namespace

void ToolRegistry::add(std::shared_ptr<Tool> tool) {
  if (tool) tools_.push_back(std::move(tool));
}

const Tool* ToolRegistry::find(const std::string& name) const {
  for (const auto& tool : tools_) {
    if (tool->name() == name) return tool.get();
  }
  return nullptr;
}

std::vector<client::ToolDefinition> ToolRegistry::definitions() const {
  std::vector<client::ToolDefinition> result;
  result.reserve(tools_.size());
  for (const auto& tool : tools_) {
    result.push_back({tool->name(), tool->description(), tool->parameters()});
  }
  return result;
}

ToolResult ToolRegistry::execute(const std::string& name,
                                 const std::string& arguments,
                                 const ToolContext& context) const {
  const Tool* tool = find(name);
  if (tool == nullptr) return failure("unknown tool: " + name);
  try {
    ToolResult result = tool->execute(arguments, context);
    if (result.content.size() > context.max_output_bytes) {
      result.content.resize(
          utf8_boundary(result.content, context.max_output_bytes));
      result.content += "\n[output truncated]";
    }
    return result;
  } catch (const std::exception& exc) {
    return failure(std::string("tool failed: ") + exc.what());
  }
}

ToolRegistry make_builtin_tools() {
  ToolRegistry registry;
  registry.add(std::make_shared<ListFilesTool>());
  registry.add(std::make_shared<SearchTool>());
  registry.add(std::make_shared<ReadFileTool>());
  registry.add(std::make_shared<WriteFileTool>());
  registry.add(std::make_shared<ExecCommandTool>());
  return registry;
}

std::string load_agents_md(const fs::path& workspace, std::size_t max_bytes) {
  std::error_code ec;
  const fs::path root = fs::weakly_canonical(workspace, ec);
  if (ec) return {};
  const fs::path target = fs::weakly_canonical(root / "AGENTS.md", ec);
  if (ec || !within_root(root, target)) return {};
  if (!fs::is_regular_file(target, ec)) return {};
  std::ifstream input(target, std::ios::binary);
  if (!input) return {};
  std::string data(max_bytes + 1, '\0');
  input.read(data.data(), static_cast<std::streamsize>(data.size()));
  data.resize(static_cast<std::size_t>(input.gcount()));
  if (data.size() > max_bytes) {
    data.resize(max_bytes);
    data += "\n[AGENTS.md truncated]\n";
  }
  return data;
}

}  // namespace taia::tools
