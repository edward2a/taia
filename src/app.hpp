#pragma once

#include <memory>

#include "cpptui.hpp"
#include "ui/actions.hpp"
#include "ui/chat_ui.hpp"

namespace taia {

class App {
 public:
  int run();

 private:
  void handle_action(const ui::Action& action);

  cpptui::App tui_;
  std::unique_ptr<ui::ChatUi> chat_;
};

}
