#include "controller/session_controller.hpp"

#include <chrono>
#include <exception>
#include <utility>

namespace taia {

namespace {

constexpr std::size_t kMaxHistoryBytes = 512 * 1024;
constexpr std::size_t kMaxToolOutputPerTurn = 256 * 1024;

std::size_t message_bytes(const client::ChatMessage& message) {
  std::size_t total = message.role.size() + message.content.size() +
                      message.tool_call_id.size() + 32;
  for (const client::ToolCall& call : message.tool_calls) {
    total += call.id.size() + call.name.size() + call.arguments.size() + 16;
  }
  return total;
}

}

SessionController::SessionController(std::string base_url, std::string api_key,
                                     net::StreamTimeouts timeouts,
                                     std::string workspace)
    : client_(std::move(base_url), std::move(api_key), timeouts),
      tools_(tools::make_builtin_tools()) {
  std::error_code ec;
  workspace_ = workspace.empty() ? std::filesystem::current_path(ec)
                                 : std::filesystem::path(workspace);
  if (!ec) {
    const auto canonical = std::filesystem::weakly_canonical(workspace_, ec);
    if (!ec) workspace_ = canonical;
  }
  system_prompt_ =
      "You are a coding agent working in " + workspace_.string() +
      ". Use the provided read-only tools to inspect the workspace. "
      "Use workspace-relative paths and do not claim to modify files.";
  const std::string agents = tools::load_agents_md(workspace_);
  if (!agents.empty()) {
    system_prompt_ += "\n\nAGENTS.md:\n";
    system_prompt_ += agents;
  }
}

SessionController::~SessionController() { stop(); }

void SessionController::start() {
  if (started_) return;
  stopping_ = false;
  cancel_ = false;
  started_ = true;
  worker_ = std::thread([this] { worker_loop(); });
}

void SessionController::stop() {
  if (!started_) return;
  {
    std::lock_guard<std::mutex> lock(task_mutex_);
    stopping_ = true;
    cancel_ = true;
  }
  task_cv_.notify_all();
  if (worker_.joinable()) worker_.join();
  started_ = false;
}

void SessionController::enqueue(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lock(task_mutex_);
    tasks_.push_back(std::move(task));
  }
  task_cv_.notify_one();
}

void SessionController::worker_loop() {
  while (true) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(task_mutex_);
      task_cv_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
      if (stopping_) break;
      task = std::move(tasks_.front());
      tasks_.pop_front();
    }
    try {
      task();
    } catch (const std::exception& exc) {
      push_event({ControllerEvent::Type::Error, {},
                  std::string("internal error: ") + exc.what(), false});
    } catch (...) {
      push_event({ControllerEvent::Type::Error, {}, "internal error", false});
    }
  }
}

void SessionController::push_event(ControllerEvent event) {
  {
    std::lock_guard<std::mutex> lock(event_mutex_);
    events_.push_back(std::move(event));
  }
}

bool SessionController::poll_event(ControllerEvent& event) {
  std::lock_guard<std::mutex> lock(event_mutex_);
  if (events_.empty()) return false;
  event = std::move(events_.front());
  events_.pop_front();
  return true;
}

bool SessionController::begin_request() {
  std::lock_guard<std::mutex> lock(task_mutex_);
  if (stopping_) return false;
  cancel_ = false;
  return true;
}

void SessionController::set_selected_model(const std::string& model) {
  selected_model_ = model;
}

void SessionController::refresh_models() {
  enqueue([this] { do_refresh_models(); });
}

void SessionController::load_model() {
  const std::string model = selected_model_;
  enqueue([this, model] { do_load_model(model); });
}

void SessionController::unload_model() {
  const std::string model = selected_model_;
  enqueue([this, model] { do_unload_model(model); });
}

void SessionController::submit_prompt(const std::string& text) {
  const std::string model = selected_model_;
  enqueue([this, model, text] { do_submit_prompt(model, text); });
}

void SessionController::cancel_stream() { cancel_ = true; }

void SessionController::do_refresh_models() {
  if (!begin_request()) return;
  std::vector<client::ModelInfo> models;
  std::string error;
  if (!client_.list_models(models, error, &cancel_)) {
    push_event({ControllerEvent::Type::Error, {}, error, false});
    return;
  }
  ControllerEvent event;
  event.type = ControllerEvent::Type::ModelsLoaded;
  event.models = std::move(models);
  event.ok = true;
  push_event(std::move(event));
}

void SessionController::do_load_model(const std::string& model) {
  if (model.empty()) {
    push_event({ControllerEvent::Type::Error, {}, "no model selected", false});
    return;
  }
  if (!begin_request()) return;
  std::string error;
  if (!client_.load_model(model, error, &cancel_)) {
    push_event({ControllerEvent::Type::Error, {}, error, false});
    return;
  }
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (!stopping_ && std::chrono::steady_clock::now() < deadline) {
    std::vector<client::ModelInfo> models;
    std::string list_error;
    if (!client_.list_models(models, list_error, &cancel_)) {
      push_event({ControllerEvent::Type::Error, {}, list_error, false});
      return;
    }
    for (const client::ModelInfo& info : models) {
      if (info.id != model) continue;
      if (info.status == "loaded") {
        push_event({ControllerEvent::Type::LoadFinished, {}, model, true});
        return;
      }
      if (info.status == "unloaded" && info.failed) {
        std::string message = "failed to load " + model;
        if (info.exit_code != 0) {
          message += " (exit code " + std::to_string(info.exit_code) + ")";
        }
        push_event({ControllerEvent::Type::Error, {}, message, false});
        return;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (!stopping_) {
    push_event(
        {ControllerEvent::Type::Error, {}, "timed out loading " + model, false});
  }
}

void SessionController::do_unload_model(const std::string& model) {
  if (model.empty()) {
    push_event({ControllerEvent::Type::Error, {}, "no model selected", false});
    return;
  }
  if (!begin_request()) return;
  std::string error;
  if (!client_.unload_model(model, error, &cancel_)) {
    push_event({ControllerEvent::Type::Error, {}, error, false});
    return;
  }
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!stopping_ && std::chrono::steady_clock::now() < deadline) {
    std::vector<client::ModelInfo> models;
    std::string list_error;
    if (!client_.list_models(models, list_error, &cancel_)) {
      push_event({ControllerEvent::Type::Error, {}, list_error, false});
      return;
    }
    bool found = false;
    for (const client::ModelInfo& info : models) {
      if (info.id != model) continue;
      found = true;
      if (info.status == "unloaded") {
        push_event({ControllerEvent::Type::UnloadFinished, {}, model, true});
        return;
      }
    }
    if (!found) {
      push_event({ControllerEvent::Type::UnloadFinished, {}, model, true});
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (!stopping_) {
    push_event(
        {ControllerEvent::Type::Error, {}, "timed out unloading " + model, false});
  }
}

void SessionController::trim_history() {
  std::size_t total = system_prompt_.size();
  for (const client::ChatMessage& message : history_) {
    total += message_bytes(message);
  }
  while (total > kMaxHistoryBytes && history_.size() > 1) {
    std::size_t next_user = 1;
    while (next_user < history_.size() &&
           history_[next_user].role != "user") {
      ++next_user;
    }
    if (next_user >= history_.size()) break;
    for (std::size_t i = 0; i < next_user; ++i) {
      total -= message_bytes(history_[i]);
    }
    history_.erase(history_.begin(), history_.begin() + next_user);
  }
}

void SessionController::do_submit_prompt(const std::string& model,
                                         const std::string& text) {
  if (model.empty()) {
    push_event({ControllerEvent::Type::Error, {}, "no model selected", false});
    return;
  }
  if (!begin_request()) return;
  history_.push_back({"user", text});
  const auto definitions = tools_.definitions();
  constexpr int kMaxRounds = 8;
  constexpr std::size_t kMaxToolCalls = 8;
  std::size_t tool_output_bytes = 0;
  for (int round = 0; round < kMaxRounds; ++round) {
    if (stopping_) return;
    if (cancel_) {
      push_event({ControllerEvent::Type::StreamDone, {}, "", true});
      return;
    }
    trim_history();
    std::vector<client::ChatMessage> request;
    request.reserve(history_.size() + 1);
    request.push_back({"system", system_prompt_});
    request.insert(request.end(), history_.begin(), history_.end());
    client::ChatResult result;
    std::string error;
    const bool ok = client_.chat_turn(
        model, request, definitions,
        [this](const std::string& delta) {
          push_event({ControllerEvent::Type::Delta, {}, delta, true});
        },
        result, error, &cancel_);
    if (!ok) {
      if (stopping_) return;
      if (error == "cancelled") {
        push_event({ControllerEvent::Type::StreamDone, {}, "", true});
        return;
      }
      push_event({ControllerEvent::Type::Error, {}, error, false});
      return;
    }
    if (result.tool_calls.empty()) {
      history_.push_back({"assistant", result.content});
      push_event({ControllerEvent::Type::StreamDone, {}, "", true});
      return;
    }
    if (result.tool_calls.size() > kMaxToolCalls) {
      push_event({ControllerEvent::Type::Error, {},
                  "too many tool calls in one round", false});
      return;
    }
    const std::size_t history_before_assistant = history_.size();
    client::ChatMessage assistant;
    assistant.role = "assistant";
    assistant.content = result.content;
    assistant.tool_calls = result.tool_calls;
    history_.push_back(std::move(assistant));
    for (const client::ToolCall& call : result.tool_calls) {
      if (stopping_) {
        history_.resize(history_before_assistant);
        return;
      }
      if (cancel_) {
        history_.resize(history_before_assistant);
        push_event({ControllerEvent::Type::StreamDone, {}, "", true});
        return;
      }
      push_event({ControllerEvent::Type::ToolStarted, {},
                  call.name + " " + call.arguments, true});
      const tools::ToolContext context{workspace_, &cancel_, 32768};
      const tools::ToolResult tool_result =
          tools_.execute(call.name, call.arguments, context);
      std::string output = tool_result.content;
      if (!tool_result.ok) output = "error: " + output;
      const std::size_t remaining =
          tool_output_bytes < kMaxToolOutputPerTurn
              ? kMaxToolOutputPerTurn - tool_output_bytes
              : 0;
      if (output.size() > remaining) {
        output.resize(remaining);
        output += "\n[truncated]";
      }
      tool_output_bytes += output.size();
      push_event(
          {ControllerEvent::Type::ToolFinished, {}, output, tool_result.ok});
      client::ChatMessage tool;
      tool.role = "tool";
      tool.content = output;
      tool.tool_call_id = call.id;
      history_.push_back(std::move(tool));
    }
  }
  push_event(
      {ControllerEvent::Type::Error, {}, "tool round limit reached", false});
}

}
