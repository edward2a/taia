#include "controller/session_controller.hpp"

#include <chrono>
#include <utility>

namespace taia {

SessionController::SessionController(std::string base_url, std::string api_key,
                                     net::StreamTimeouts timeouts)
    : client_(std::move(base_url), std::move(api_key), timeouts) {}

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
    task();
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

void SessionController::do_submit_prompt(const std::string& model,
                                         const std::string& text) {
  if (model.empty()) {
    push_event({ControllerEvent::Type::Error, {}, "no model selected", false});
    return;
  }
  if (!begin_request()) return;
  history_.push_back({"user", text});
  std::string reply;
  std::string error;
  const bool ok = client_.chat_stream(
      model, history_,
      [this, &reply](const std::string& delta) {
        reply += delta;
        push_event({ControllerEvent::Type::Delta, {}, delta, true});
      },
      error, &cancel_);
  if (!ok) {
    push_event({ControllerEvent::Type::Error, {}, error, false});
    return;
  }
  history_.push_back({"assistant", reply});
  push_event({ControllerEvent::Type::StreamDone, {}, "", true});
}

}
