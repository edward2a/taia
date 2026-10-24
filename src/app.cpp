#include "app.hpp"

namespace taia {

void App::build_ui() {
  cpptui::Theme::set_theme(cpptui::Theme::Dark());

  auto root = std::make_shared<cpptui::Vertical>();
  root->add(std::make_shared<cpptui::Label>(cpptui::StyledText("taia-ct")));
  root->add(std::make_shared<cpptui::VerticalSpacer>(1));
  root->add(std::make_shared<cpptui::Label>(
      cpptui::StyledText("skeleton ready - press q to quit")));

  root_ = root;
}

int App::run() {
  build_ui();
  tui_.register_exit_key('q');
  tui_.run(root_);
  return 0;
}

}
