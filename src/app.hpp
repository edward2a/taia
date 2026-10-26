#pragma once

#include <memory>
#include <string>

#include "controller/session_controller.hpp"
#include "cpptui.hpp"
#include "ui/actions.hpp"
#include "ui/chat_ui.hpp"

namespace taia {

class App {
 public:
  int run(std::string base_url, std::string api_key = {});

 private:
  void handle_action(const ui::Action& action);
  void drain_events();

  cpptui::App tui_;
  std::unique_ptr<ui::ChatUi> chat_;
  std::unique_ptr<SessionController> controller_;
  std::string selected_model_;
  bool awaiting_initial_models_ = false;
  bool assistant_open_ = false;
  cpptui::TimerId poll_timer_{};
};

}
