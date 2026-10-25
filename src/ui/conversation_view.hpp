#pragma once

#include <string>
#include <vector>

#include "cpptui.hpp"

namespace taia::ui {

class ConversationView : public cpptui::Widget {
 public:
  ConversationView();

  void add_message(const std::string& role, const std::string& text);
  void append_to_last(const std::string& text);
  void clear();

  void render(cpptui::Buffer& buffer) override;
  bool on_event(const cpptui::Event& event) override;

 private:
  struct Line {
    std::string text;
    cpptui::Color color;
    bool bold = false;
  };

  void rebuild_lines(int width);
  int max_scroll() const;

  std::vector<std::pair<std::string, std::string>> messages_;
  std::vector<Line> lines_;
  int scroll_offset_ = 0;
  bool stick_to_bottom_ = true;
};

}
