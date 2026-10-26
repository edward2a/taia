#include "ui/approval_view.hpp"

#include <algorithm>

namespace taia::ui {

namespace {

std::string expand_tabs(const std::string& line) {
  std::string out;
  for (char c : line) {
    if (c == '\t') {
      out.append(4, ' ');
    } else if (c != '\r') {
      out.push_back(c);
    }
  }
  return out;
}

void draw_text(cpptui::Buffer& buffer, int x, int y, const std::string& text,
               cpptui::Color fg, cpptui::Color bg, int max_width) {
  int col = 0;
  for (const auto& unit : cpptui::TextHelper::prepare_text_for_render(text)) {
    if (col + unit.display_width > max_width) break;
    cpptui::Cell cell;
    cell.content = unit.content;
    cell.fg_color = fg;
    cell.bg_color = bg;
    buffer.set(x + col, y, cell);
    if (unit.display_width == 2 && col + 1 < max_width) {
      cpptui::Cell skip;
      skip.content = "";
      skip.fg_color = fg;
      skip.bg_color = bg;
      buffer.set(x + col + 1, y, skip);
    }
    col += unit.display_width;
  }
}

void draw_line(cpptui::Buffer& buffer, int x, int y, const std::string& text,
               cpptui::Color fg, cpptui::Color bg, int max_width,
               int start_col, bool& clipped_left, bool& clipped_right) {
  int display_col = 0;
  int draw_col = 0;
  for (const auto& unit : cpptui::TextHelper::prepare_text_for_render(text)) {
    if (display_col + unit.display_width <= start_col) {
      display_col += unit.display_width;
      continue;
    }
    if (display_col < start_col) {
      display_col += unit.display_width;
      continue;
    }
    if (draw_col + unit.display_width > max_width) {
      clipped_right = true;
      break;
    }
    cpptui::Cell cell;
    cell.content = unit.content;
    cell.fg_color = fg;
    cell.bg_color = bg;
    buffer.set(x + draw_col, y, cell);
    if (unit.display_width == 2 && draw_col + 1 < max_width) {
      cpptui::Cell skip;
      skip.content = "";
      skip.fg_color = fg;
      skip.bg_color = bg;
      buffer.set(x + draw_col + 1, y, skip);
    }
    display_col += unit.display_width;
    draw_col += unit.display_width;
  }
  if (start_col > 0) clipped_left = true;
}

}

ApprovalView::ApprovalView() {
  focusable = true;
  tab_stop = false;
}

void ApprovalView::set_content(const std::string& text) {
  lines_.clear();
  std::string line;
  for (char c : text) {
    if (c == '\n') {
      lines_.push_back(expand_tabs(line));
      line.clear();
    } else if (c != '\r') {
      line.push_back(c);
    }
  }
  if (!line.empty() || (!text.empty() && text.back() == '\n')) {
    lines_.push_back(expand_tabs(line));
  }
  if (lines_.empty()) lines_.push_back("(no preview)");
  scroll_ = 0;
  scroll_x_ = 0;
}

int ApprovalView::max_scroll() const {
  const int visible = std::max(1, height - 2);
  return std::max(0, static_cast<int>(lines_.size()) - visible);
}

int ApprovalView::max_line_width() const {
  int max_width = 0;
  for (const std::string& line : lines_) {
    max_width = std::max(max_width, cpptui::utf8_display_width(line));
  }
  return max_width;
}

bool ApprovalView::on_event(const cpptui::Event& event) {
  if (!visible) return false;
  if (event.is_mouse_event()) {
    if (!event.mouse_wheel()) return false;
    if (event.mouse_wheel_up()) {
      scroll_ = std::max(0, scroll_ - 3);
    } else {
      scroll_ = std::min(max_scroll(), scroll_ + 3);
    }
    return true;
  }
  if (!event.is_key_event()) return false;
  if (event.key == 'y' || event.key == 'Y') {
    if (on_decision) on_decision(true);
    return true;
  }
  if (event.key == 'n' || event.key == 'N' || event.is_escape()) {
    if (on_decision) on_decision(false);
    return true;
  }
  if (event.is_nav_up()) {
    scroll_ = std::max(0, scroll_ - 1);
    return true;
  }
  if (event.is_nav_down()) {
    scroll_ = std::min(max_scroll(), scroll_ + 1);
    return true;
  }
  if (event.is_nav_pgup()) {
    scroll_ = std::max(0, scroll_ - std::max(1, height - 2));
    return true;
  }
  if (event.is_nav_pgdn()) {
    scroll_ = std::min(max_scroll(), scroll_ + std::max(1, height - 2));
    return true;
  }
  if (event.is_nav_left()) {
    scroll_x_ = std::max(0, scroll_x_ - 4);
    return true;
  }
  if (event.is_nav_right()) {
    scroll_x_ = std::min(std::max(0, max_line_width() - width), scroll_x_ + 4);
    return true;
  }
  if (event.is_nav_home()) {
    scroll_ = 0;
    scroll_x_ = 0;
    return true;
  }
  if (event.is_nav_end()) {
    scroll_ = max_scroll();
    scroll_x_ = std::max(0, max_line_width() - width);
    return true;
  }
  return false;
}

void ApprovalView::render(cpptui::Buffer& buffer) {
  const auto& theme = cpptui::Theme::current();
  for (int row = 0; row < height; ++row) {
    for (int col = 0; col < width; ++col) {
      cpptui::Cell cell;
      cell.content = " ";
      cell.fg_color = theme.panel_fg;
      cell.bg_color = theme.panel_bg;
      buffer.set(x + col, y + row, cell);
    }
  }
  if (width <= 0 || height <= 0) return;

  const std::string header =
      "APPROVAL REQUIRED  [y] approve  [n] reject  (arrows scroll)";
  draw_text(buffer, x, y, header, theme.warning, theme.panel_bg,
            std::min(width, static_cast<int>(header.size())));
  if (height < 2) return;
  for (int col = 0; col < width; ++col) {
    cpptui::Cell cell;
    cell.content = " ";
    cell.bg_color = theme.border;
    cell.fg_color = theme.border;
    buffer.set(x + col, y + 1, cell);
  }

  if (scroll_ > max_scroll()) scroll_ = max_scroll();
  const int max_x = std::max(0, max_line_width() - width);
  scroll_x_ = std::clamp(scroll_x_, 0, max_x);
  const int visible = height - 2;
  const int total = static_cast<int>(lines_.size());
  for (int row = 0; row < visible; ++row) {
    const int index = scroll_ + row;
    if (index >= total) break;
    bool clipped_left = false;
    bool clipped_right = false;
    draw_line(buffer, x, y + 2 + row, lines_[index], theme.panel_fg,
              theme.panel_bg, width, scroll_x_, clipped_left, clipped_right);
    if (clipped_left) {
      cpptui::Cell cell;
      cell.content = "<";
      cell.fg_color = theme.warning;
      cell.bg_color = theme.panel_bg;
      buffer.set(x, y + 2 + row, cell);
    }
    if (clipped_right && width > 0) {
      cpptui::Cell cell;
      cell.content = ">";
      cell.fg_color = theme.warning;
      cell.bg_color = theme.panel_bg;
      buffer.set(x + width - 1, y + 2 + row, cell);
    }
  }
  if (total > 0 && width > 0) {
    const std::string position =
        " " + std::to_string(scroll_ + 1) + "/" + std::to_string(total) +
        " col " + std::to_string(scroll_x_) + " ";
    const int start = std::max(0, width - static_cast<int>(position.size()));
    draw_text(buffer, x + start, y, position, theme.secondary, theme.panel_bg,
              width - start);
  }
}

}
