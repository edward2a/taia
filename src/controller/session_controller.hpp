#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
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
    ApprovalRequest,
    StreamDone,
    Error,
  };

  Type type = Type::Error;
  std::vector<client::ModelInfo> models;
  std::string text;
  bool ok = true;
  std::uint64_t request_id = 0;
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
  void resolve_approval(std::uint64_t id, bool approved);

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
  std::size_t trim_history();
  void drop_last_turn();
  bool request_approval(const std::string& preview);

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

  std::mutex approval_mutex_;
  std::condition_variable approval_cv_;
  bool approval_pending_ = false;
  bool approval_approved_ = false;
  std::uint64_t approval_id_ = 0;
  std::uint64_t approval_counter_ = 0;
};

}
