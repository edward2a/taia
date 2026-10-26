#include "tools/tools.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>
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

ToolResult failure(const std::string& message) { return {false, message}; }

bool cancelled(const ToolContext& context) {
  return context.cancel != nullptr && context.cancel->load();
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
  auto root_it = root.begin();
  auto candidate_it = candidate.begin();
  for (; root_it != root.end(); ++root_it, ++candidate_it) {
    if (candidate_it == candidate.end() || *candidate_it != *root_it) {
      error = "path escapes workspace";
      return false;
    }
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

std::string truncate_line(const std::string& line) {
  if (line.size() <= kMaxLineChars) return line;
  return line.substr(0, kMaxLineChars) + "...";
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
      result.content.resize(context.max_output_bytes);
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
  return registry;
}

std::string load_agents_md(const fs::path& workspace, std::size_t max_bytes) {
  std::ifstream input(workspace / "AGENTS.md", std::ios::binary);
  if (!input) return {};
  std::string data((std::istreambuf_iterator<char>(input)),
                   std::istreambuf_iterator<char>());
  if (data.size() > max_bytes) {
    data.resize(max_bytes);
    data += "\n[AGENTS.md truncated]\n";
  }
  return data;
}

}  // namespace taia::tools
