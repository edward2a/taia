#pragma once

#include <memory>

#include "cpptui.hpp"

namespace taia {

class App {
 public:
  App();

  int run();

 private:
  void build_ui();

  cpptui::App tui_;
  std::shared_ptr<cpptui::Widget> root_;
};

}  // namespace taia
