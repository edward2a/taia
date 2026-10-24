#include "ui/prompt_input.hpp"

namespace taia::ui {

PromptInput::PromptInput() {
  show_line_numbers = false;
  show_scrollbar = false;
  word_wrap = true;
}

void PromptInput::render(cpptui::Buffer& buffer) {
  cpptui::TextArea::render(buffer);
  if (!get_text().empty() || placeholder.empty()) return;

  const auto& t = cpptui::Theme::current();
  int col = 0;
  for (const auto& ch : cpptui::TextHelper::prepare_text_for_render(placeholder)) {
    if (col >= width) break;
    cpptui::Cell cell;
    cell.content = ch.content;
    cell.fg_color = t.input_placeholder;
    cell.bg_color = t.background;
    buffer.set(x + col, y, cell);
    col += ch.display_width;
  }
}

}
