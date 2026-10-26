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

struct ChatMessage {
  std::string role;
  std::string content;
};

class LlamaClient {
 public:
  explicit LlamaClient(std::string base_url, std::string api_key = {});

  bool list_models(std::vector<ModelInfo>& models, std::string& error,
                   const std::atomic<bool>* cancel = nullptr);
  bool load_model(const std::string& model, std::string& error,
                  const std::atomic<bool>* cancel = nullptr);
  bool unload_model(const std::string& model, std::string& error,
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
