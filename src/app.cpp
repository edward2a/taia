#include "app.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace taia {

void App::handle_action(const ui::Action& action) {
  switch (action.type) {
    case ui::ActionType::SubmitPrompt:
      chat_->clear_error();
      chat_->append_message("user", action.text);
      assistant_open_ = false;
      chat_->set_state(ui::RunState::Generating);
      if (controller_) controller_->submit_prompt(action.text);
      break;
    case ui::ActionType::SelectModel:
      selected_model_ = action.text;
      if (controller_) controller_->set_selected_model(action.text);
      break;
    case ui::ActionType::LoadModel:
      chat_->clear_error();
      chat_->set_state(ui::RunState::Loading);
      if (controller_) controller_->load_model();
      break;
    case ui::ActionType::UnloadModel:
      chat_->clear_error();
      chat_->set_state(ui::RunState::Loading);
      if (controller_) controller_->unload_model();
      break;
    case ui::ActionType::RefreshModels:
      chat_->clear_error();
      if (controller_) controller_->refresh_models();
      break;
  }
}

void App::drain_events() {
  if (!controller_) return;
  ControllerEvent event;
  int budget = 256;
  while (budget-- > 0 && controller_->poll_event(event)) {
    switch (event.type) {
      case ControllerEvent::Type::ModelsLoaded: {
        std::vector<std::string> ids;
        ids.reserve(event.models.size());
        for (const client::ModelInfo& model : event.models) {
          ids.push_back(model.id);
        }
        if (std::find(ids.begin(), ids.end(), selected_model_) == ids.end()) {
          selected_model_.clear();
          for (const client::ModelInfo& model : event.models) {
            if (model.status == "loaded") {
              selected_model_ = model.id;
              break;
            }
          }
          if (selected_model_.empty()) {
            selected_model_ = ids.empty() ? "" : ids.front();
          }
        }
        chat_->set_models(ids, selected_model_);
        controller_->set_selected_model(selected_model_);
        if (awaiting_initial_models_) {
          awaiting_initial_models_ = false;
          chat_->set_state(ui::RunState::Idle);
        }
        if (event.models.empty()) {
          chat_->set_error("no models available");
        }
        break;
      }
      case ControllerEvent::Type::LoadFinished:
        chat_->set_state(ui::RunState::Idle);
        break;
      case ControllerEvent::Type::UnloadFinished:
        chat_->set_state(ui::RunState::Idle);
        break;
      case ControllerEvent::Type::Delta:
        if (!assistant_open_) {
          chat_->append_message("assistant", "");
          assistant_open_ = true;
        }
        chat_->append_to_last(event.text);
        break;
      case ControllerEvent::Type::ToolStarted: {
        assistant_open_ = false;
        std::string call = event.text;
        if (call.size() > 300) call = call.substr(0, 300) + "...";
        chat_->append_message("tool", "\u2192 " + call);
        chat_->set_state(ui::RunState::Tool);
        break;
      }
      case ControllerEvent::Type::ToolFinished: {
        std::string text = event.text;
        if (text.size() > 2000) text = text.substr(0, 2000) + "\n...";
        chat_->append_to_last("\n" + text);
        chat_->set_state(ui::RunState::Generating);
        break;
      }
      case ControllerEvent::Type::StreamDone:
        assistant_open_ = false;
        chat_->set_state(ui::RunState::Idle);
        break;
      case ControllerEvent::Type::Error:
        assistant_open_ = false;
        chat_->set_error(event.text);
        chat_->set_state(ui::RunState::Error);
        break;
    }
  }
}

int App::run(std::string base_url, std::string api_key,
             net::StreamTimeouts timeouts, std::string workspace) {
  cpptui::Theme::set_theme(cpptui::Theme::TokyoNight());
  chat_ = std::make_unique<ui::ChatUi>(
      &tui_, [this](const ui::Action& action) { handle_action(action); });
  controller_ = std::make_unique<SessionController>(
      std::move(base_url), std::move(api_key), timeouts, std::move(workspace));

  chat_->set_models({}, "");
  chat_->set_state(ui::RunState::Loading);
  chat_->set_agent("-");
  chat_->set_variant("-");
  chat_->set_context_usage("-");

  awaiting_initial_models_ = true;
  controller_->start();
  controller_->refresh_models();
  poll_timer_ = tui_.add_timer(40, [this] { drain_events(); });

  tui_.register_exit_key('q');
  tui_.register_key('c', [] { cpptui::App::quit(); }, true);
  tui_.post([this] { chat_->focus_prompt(); });
  tui_.run(chat_->root());
  controller_->stop();
  return 0;
}

}
