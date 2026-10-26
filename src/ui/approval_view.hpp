#pragma once

#include <functional>
#include <string>
#include <vector>

#include "cpptui.hpp"

namespace taia::ui {

class ApprovalView : public cpptui::Widget {
 public:
  ApprovalView();

  void set_content(const std::string& text);

  std::function<void(bool)> on_decision;

  bool on_event(const cpptui::Event& event) override;
  void render(cpptui::Buffer& buffer) override;

 private:
  int max_scroll() const;
  int max_line_width() const;

  std::vector<std::string> lines_;
  int scroll_ = 0;
  int scroll_x_ = 0;
};

}
