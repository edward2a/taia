#include "ui/conversation_view.hpp"

#include <algorithm>
#include <sstream>

namespace taia::ui {

namespace {

cpptui::Color role_color(const std::string& role) {
  const auto& t = cpptui::Theme::current();
  if (role == "user") return t.primary;
  if (role == "assistant") return t.success;
  if (role == "system") return t.secondary;
  if (role == "tool") return t.warning;
  if (role == "error") return t.error;
  return t.foreground;
}

std::vector<std::string> wrap_columns(const std::string& text, int width) {
  std::vector<std::string> lines;
  if (width <= 0) {
    lines.push_back(text);
    return lines;
  }

  std::vector<std::string> paragraphs;
  std::string paragraph;
  for (char c : text) {
    if (c == '\n') {
      paragraphs.push_back(paragraph);
      paragraph.clear();
    } else {
      paragraph.push_back(c);
    }
  }
  paragraphs.push_back(paragraph);

  for (const auto& para : paragraphs) {
    std::istringstream words(para);
    std::string word;
    std::string line;
    int line_width = 0;
    while (words >> word) {
      int word_width = cpptui::utf8_display_width(word);
      if (!line.empty() && line_width + 1 + word_width <= width) {
        line += " " + word;
        line_width += 1 + word_width;
        continue;
      }
      if (!line.empty()) {
        lines.push_back(line);
        line.clear();
        line_width = 0;
      }
      if (word_width <= width) {
        line = word;
        line_width = word_width;
        continue;
      }
      std::string part;
      int part_width = 0;
      for (const auto& unit :
           cpptui::TextHelper::prepare_text_for_render(word)) {
        if (part_width + unit.display_width > width && !part.empty()) {
          lines.push_back(part);
          part.clear();
          part_width = 0;
        }
        part += unit.content;
        part_width += unit.display_width;
      }
      line = part;
      line_width = part_width;
    }
    lines.push_back(line);
  }
  return lines;
}

}

ConversationView::ConversationView() {
  focusable = true;
  tab_stop = false;
}

void ConversationView::add_message(const std::string& role,
                                   const std::string& text) {
  messages_.emplace_back(role, text);
  stick_to_bottom_ = true;
}

void ConversationView::clear() {
  messages_.clear();
  lines_.clear();
  scroll_offset_ = 0;
  stick_to_bottom_ = true;
}

void ConversationView::rebuild_lines(int content_width) {
  lines_.clear();
  if (content_width <= 0) return;

  const auto& t = cpptui::Theme::current();
  for (const auto& [role, text] : messages_) {
    if (!lines_.empty()) lines_.push_back({"", t.foreground, false});
    lines_.push_back({role + ":", role_color(role), true});

    for (const auto& wrapped : wrap_columns(text, content_width)) {
      lines_.push_back({wrapped, t.foreground, false});
    }
  }
}

int ConversationView::max_scroll() const {
  return std::max(0, static_cast<int>(lines_.size()) - height);
}

void ConversationView::render(cpptui::Buffer& buffer) {
  rebuild_lines(width);
  if (stick_to_bottom_) scroll_offset_ = max_scroll();
  scroll_offset_ = std::clamp(scroll_offset_, 0, max_scroll());

  const auto& t = cpptui::Theme::current();
  for (int row = 0; row < height; ++row) {
    int line_index = scroll_offset_ + row;
    bool has_line = line_index >= 0 &&
                    line_index < static_cast<int>(lines_.size());

    std::string text = has_line ? lines_[line_index].text : "";
    cpptui::Color fg = has_line ? lines_[line_index].color : t.foreground;
    bool bold = has_line && lines_[line_index].bold;

    int col = 0;
    for (const auto& ch : cpptui::TextHelper::prepare_text_for_render(text)) {
      if (col >= width) break;
      if (ch.display_width == 0) {
        if (col > 0) {
          cpptui::Cell previous = buffer.get(x + col - 1, y + row);
          previous.content += ch.content;
          buffer.set(x + col - 1, y + row, previous);
        }
        continue;
      }
      cpptui::Cell cell;
      cell.content = ch.content;
      cell.fg_color = fg;
      cell.bg_color = t.panel_bg;
      cell.bold = bold;
      buffer.set(x + col, y + row, cell);
      if (ch.display_width == 2 && col + 1 < width) {
        cpptui::Cell continuation;
        continuation.content = "";
        continuation.bg_color = t.panel_bg;
        buffer.set(x + col + 1, y + row, continuation);
      }
      col += ch.display_width;
    }
    for (; col < width; ++col) {
      cpptui::Cell cell;
      cell.content = " ";
      cell.bg_color = t.panel_bg;
      buffer.set(x + col, y + row, cell);
    }
  }
}

bool ConversationView::on_event(const cpptui::Event& event) {
  rebuild_lines(width);
  int page = std::max(1, height - 1);
  int delta = 0;

  if (event.is_mouse_event() && event.mouse_wheel()) {
    if (!contains(event.x, event.y)) return false;
    delta = event.mouse_wheel_up() ? -3 : 3;
  } else if (event.is_key_event() && has_focus_within()) {
    if (event.is_nav_pgup()) {
      delta = -page;
    } else if (event.is_nav_pgdn()) {
      delta = page;
    } else if (event.is_nav_up()) {
      delta = -1;
    } else if (event.is_nav_down()) {
      delta = 1;
    } else if (event.is_nav_home()) {
      scroll_offset_ = 0;
      stick_to_bottom_ = false;
      return true;
    } else if (event.is_nav_end()) {
      scroll_offset_ = max_scroll();
      stick_to_bottom_ = true;
      return true;
    } else {
      return false;
    }
  } else {
    return false;
  }

  scroll_offset_ = std::clamp(scroll_offset_ + delta, 0, max_scroll());
  stick_to_bottom_ = scroll_offset_ >= max_scroll();
  return true;
}

}
