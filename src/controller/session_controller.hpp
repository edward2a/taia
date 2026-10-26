#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "client/llama_client.hpp"
#include "tools/tools.hpp"

namespace taia {

struct ControllerEvent {
  enum class Type {
    ModelsLoaded,
    LoadFinished,
    UnloadFinished,
    Delta,
    ToolStarted,
    ToolFinished,
    StreamDone,
    Error,
  };

  Type type = Type::Error;
  std::vector<client::ModelInfo> models;
  std::string text;
  bool ok = true;
};

class SessionController {
 public:
  explicit SessionController(std::string base_url, std::string api_key = {},
                             net::StreamTimeouts timeouts = {},
                             std::string workspace = {});
  ~SessionController();

  SessionController(const SessionController&) = delete;
  SessionController& operator=(const SessionController&) = delete;

  void start();
  void stop();

  void set_selected_model(const std::string& model);
  const std::string& selected_model() const { return selected_model_; }

  void refresh_models();
  void load_model();
  void unload_model();
  void submit_prompt(const std::string& text);
  void cancel_stream();

  bool poll_event(ControllerEvent& event);

 private:
  void enqueue(std::function<void()> task);
  void worker_loop();
  void push_event(ControllerEvent event);
  bool begin_request();

  void do_refresh_models();
  void do_load_model(const std::string& model);
  void do_unload_model(const std::string& model);
  void do_submit_prompt(const std::string& model, const std::string& text);
  void trim_history();

  client::LlamaClient client_;
  tools::ToolRegistry tools_;
  std::filesystem::path workspace_;
  std::string system_prompt_;
  std::string selected_model_;
  std::vector<client::ChatMessage> history_;

  std::atomic<bool> stopping_{false};
  std::atomic<bool> cancel_{false};

  std::thread worker_;
  std::mutex task_mutex_;
  std::condition_variable task_cv_;
  std::deque<std::function<void()>> tasks_;
  bool started_ = false;

  std::mutex event_mutex_;
  std::deque<ControllerEvent> events_;
};

}
