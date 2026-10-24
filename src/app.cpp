#include "app.hpp"

namespace taia {

void App::handle_action(const ui::Action& action) {
  switch (action.type) {
    case ui::ActionType::SubmitPrompt:
      chat_->clear_error();
      chat_->append_message("user", action.text);
      chat_->append_message("system", "(controller not wired yet)");
      break;
    case ui::ActionType::SelectModel:
      chat_->set_model_status("selected " + action.text);
      break;
    case ui::ActionType::LoadModel:
      chat_->set_busy(true);
      chat_->set_model_status("loading " + action.text);
      break;
    case ui::ActionType::UnloadModel:
      chat_->set_busy(false);
      chat_->set_model_status("unloaded " + action.text);
      break;
    case ui::ActionType::RefreshModels:
      chat_->set_models({"mock/small", "mock/large"}, "");
      break;
  }
}

int App::run() {
  cpptui::Theme::set_theme(cpptui::Theme::Dark());
  chat_ = std::make_unique<ui::ChatUi>(
      &tui_, [this](const ui::Action& action) { handle_action(action); });

  chat_->set_models({"mock/small", "mock/large"}, "mock/small");
  chat_->set_model_status("idle");

  tui_.register_exit_key('q');
  tui_.register_key('c', [] { cpptui::App::quit(); }, true);
  tui_.post([this] { chat_->focus_prompt(); });
  tui_.run(chat_->root());
  return 0;
}

}
