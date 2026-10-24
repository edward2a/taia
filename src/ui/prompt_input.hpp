#pragma once

#include <string>

#include "cpptui.hpp"

namespace taia::ui {

class PromptInput : public cpptui::TextArea {
 public:
  PromptInput();

  std::string placeholder;

  void render(cpptui::Buffer& buffer) override;
};

}
