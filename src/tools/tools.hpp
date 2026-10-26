#pragma once

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "client/llama_client.hpp"

namespace taia::tools {

struct ToolResult {
  bool ok = true;
  std::string content;
};

struct ToolContext {
  std::filesystem::path workspace;
  const std::atomic<bool>* cancel = nullptr;
  std::size_t max_output_bytes = 65536;
  std::function<bool(const std::string&)> approve;
};

class Tool {
 public:
  virtual ~Tool() = default;
  virtual std::string name() const = 0;
  virtual std::string description() const = 0;
  virtual std::string parameters() const = 0;
  virtual ToolResult execute(const std::string& arguments,
                             const ToolContext& context) const = 0;
};

class ToolRegistry {
 public:
  void add(std::shared_ptr<Tool> tool);
  const Tool* find(const std::string& name) const;
  std::vector<client::ToolDefinition> definitions() const;
  ToolResult execute(const std::string& name, const std::string& arguments,
                     const ToolContext& context) const;

 private:
  std::vector<std::shared_ptr<Tool>> tools_;
};

ToolRegistry make_builtin_tools();

std::string load_agents_md(const std::filesystem::path& workspace,
                           std::size_t max_bytes = 32768);

}
