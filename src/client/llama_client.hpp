#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "net/http_client.hpp"

namespace taia::client {

struct ModelInfo {
  std::string id;
  std::string status;
  bool failed = false;
  int exit_code = 0;
};

struct ToolCall {
  std::string id;
  std::string name;
  std::string arguments;
};

struct ChatMessage {
  std::string role;
  std::string content;
  std::vector<ToolCall> tool_calls;
  std::string tool_call_id;
};

struct ToolDefinition {
  std::string name;
  std::string description;
  std::string parameters;
};

struct ChatResult {
  std::string content;
  std::vector<ToolCall> tool_calls;
  std::string finish_reason;
};

class LlamaClient {
 public:
  explicit LlamaClient(std::string base_url, std::string api_key = {},
                       net::StreamTimeouts timeouts = {});

  bool list_models(std::vector<ModelInfo>& models, std::string& error,
                   const std::atomic<bool>* cancel = nullptr);
  bool load_model(const std::string& model, std::string& error,
                  const std::atomic<bool>* cancel = nullptr);
  bool unload_model(const std::string& model, std::string& error,
                    const std::atomic<bool>* cancel = nullptr);
  bool chat_turn(const std::string& model,
                 const std::vector<ChatMessage>& messages,
                 const std::vector<ToolDefinition>& tools,
                 const std::function<void(const std::string&)>& on_delta,
                 ChatResult& result, std::string& error,
                 const std::atomic<bool>* cancel = nullptr);
  bool chat_stream(const std::string& model,
                   const std::vector<ChatMessage>& messages,
                   const std::function<void(const std::string&)>& on_delta,
                   std::string& error,
                   const std::atomic<bool>* cancel = nullptr);

 private:
  net::HttpClient http_;
};

}
