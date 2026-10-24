#include "ui/chat_ui.hpp"

#include <algorithm>
#include <utility>

namespace taia::ui {

namespace {

std::string right_pad(const std::string& text, std::size_t width) {
  if (text.size() >= width) return text;
  return std::string(width - text.size(), ' ') + text;
}

class StatusBarRow : public cpptui::Horizontal {
 public:
  cpptui::Color bar_bg;

  void render(cpptui::Buffer& buffer) override {
    for (int dx = 0; dx < width; ++dx) {
      cpptui::Cell cell;
      cell.content = " ";
      cell.bg_color = bar_bg;
      buffer.set(x + dx, y, cell);
    }
    cpptui::Horizontal::render(buffer);
  }
};

}

ChatUi::ChatUi(cpptui::App* app, ActionSink sink)
    : app_(app), sink_(std::move(sink)) {
  root_ = std::make_shared<cpptui::Vertical>();

  conversation_ = std::make_shared<ConversationView>();
  root_->add(conversation_);

  error_label_ = std::make_shared<cpptui::Label>(cpptui::StyledText(""));
  error_label_->fixed_height = 1;
  error_label_->fg_color = cpptui::Theme::current().error;
  error_label_->visible = false;
  root_->add(error_label_);

  auto status_bar = std::make_shared<StatusBarRow>();
  status_bar->fixed_height = 1;
  status_bar->bar_bg = cpptui::Theme::current().selection;
  auto add_separator = [&] {
    auto sep = std::make_shared<cpptui::Label>(cpptui::StyledText(" | "));
    sep->fixed_width = 3;
    sep->fg_color = cpptui::Theme::current().panel_fg;
    status_bar->add(sep);
  };

  state_label_ =
      std::make_shared<cpptui::Label>(cpptui::StyledText("\u25CF IDLE"));
  state_label_->fixed_width = 12;
  status_bar->add(state_label_);
  add_separator();
  agent_label_ = std::make_shared<cpptui::Label>(cpptui::StyledText("-"));
  agent_label_->fixed_width = 32;
  status_bar->add(agent_label_);
  add_separator();
  model_dropdown_ = std::make_shared<cpptui::Dropdown>(app_);
  model_dropdown_->placeholder = "no model";
  model_dropdown_->min_width = 12;
  model_dropdown_->tab_stop = false;
  model_dropdown_->bg_color = cpptui::Theme::current().selection;
  model_dropdown_->on_change = [this](int, const std::string& value) {
    update_model_width();
    emit(ActionType::SelectModel, value);
  };
  status_bar->add(model_dropdown_);
  status_bar->add(std::make_shared<cpptui::HorizontalSpacer>(0));
  add_separator();
  variant_label_ =
      std::make_shared<cpptui::Label>(cpptui::StyledText(right_pad("-", 24)));
  variant_label_->fixed_width = 24;
  status_bar->add(variant_label_);
  add_separator();
  context_label_ =
      std::make_shared<cpptui::Label>(cpptui::StyledText(right_pad("-", 24)));
  context_label_->fixed_width = 24;
  status_bar->add(context_label_);
  root_->add(status_bar);

  auto input_area = std::make_shared<cpptui::Vertical>();
  input_area->fixed_height = 4;
  prompt_input_ = std::make_shared<PromptInput>();
  prompt_input_->placeholder = "Ask anything...";
  input_area->add(prompt_input_);
  root_->add(input_area);

  auto shortcut_bar = std::make_shared<cpptui::Horizontal>();
  shortcut_bar->fixed_height = 1;
  shortcut_bar->add(std::make_shared<cpptui::HorizontalSpacer>(1));
  std::vector<std::string> shortcuts = {"^L Load", "^U Unload", "^R Refresh",
                                        "^J Newline", "^C Quit"};
  int shortcut_width = 0;
  for (const auto& s : shortcuts) {
    shortcut_width = std::max(shortcut_width, static_cast<int>(s.size()));
  }
  for (const auto& s : shortcuts) {
    std::string text = " " + s;
    text.append(shortcut_width - static_cast<int>(s.size()), ' ');
    text += " ";
    auto item = std::make_shared<cpptui::Label>(cpptui::StyledText(text));
    item->fixed_width = static_cast<int>(text.size());
    item->bg_color = cpptui::Theme::current().primary;
    item->fg_color = cpptui::Color::contrast_color(item->bg_color);
    shortcut_bar->add(item);
    shortcut_bar->add(std::make_shared<cpptui::HorizontalSpacer>(1));
  }
  root_->add(shortcut_bar);

  app_->register_key(13, [this] {
    if (prompt_input_->has_focus()) submit_prompt();
  });
  app_->register_key(
      'l',
      [this] {
        std::string model = selected_model();
        if (!model.empty()) emit(ActionType::LoadModel, model);
      },
      true);
  app_->register_key(
      'u',
      [this] {
        std::string model = selected_model();
        if (!model.empty()) emit(ActionType::UnloadModel, model);
      },
      true);
  app_->register_key('r', [this] { emit(ActionType::RefreshModels); }, true);
}

void ChatUi::emit(ActionType type, const std::string& text) {
  if (sink_) sink_(Action{type, text});
}

std::string ChatUi::selected_model() const {
  int index = model_dropdown_->selected_index;
  if (index >= 0 && index < static_cast<int>(model_dropdown_->options.size())) {
    return model_dropdown_->options[index].plain_text();
  }
  return "";
}

void ChatUi::update_model_width() {
  std::string text = selected_model();
  if (text.empty()) text = model_dropdown_->placeholder.plain_text();
  model_dropdown_->fixed_width = static_cast<int>(text.size()) + 4;
  app_->update();
}

void ChatUi::append_message(const std::string& role, const std::string& text) {
  conversation_->add_message(role, text);
  app_->update();
}

void ChatUi::set_models(const std::vector<std::string>& ids,
                        const std::string& selected) {
  std::vector<cpptui::StyledText> options;
  options.reserve(ids.size());
  int selected_index = -1;
  for (std::size_t i = 0; i < ids.size(); ++i) {
    options.emplace_back(ids[i]);
    if (ids[i] == selected) selected_index = static_cast<int>(i);
  }
  model_dropdown_->set_options(options);
  model_dropdown_->selected_index = selected_index;
  update_model_width();
}

void ChatUi::set_state(RunState state) {
  const auto& t = cpptui::Theme::current();
  std::string label;
  cpptui::Color color;
  switch (state) {
    case RunState::Idle:
      label = "IDLE";
      color = t.foreground;
      break;
    case RunState::Loading:
      label = "LOADING";
      color = t.warning;
      break;
    case RunState::Generating:
      label = "GENERATING";
      color = t.primary;
      break;
    case RunState::Error:
      label = "ERROR";
      color = t.error;
      break;
  }
  state_label_->set_text(cpptui::StyledText(std::string("\u25CF ") + label));
  state_label_->fg_color = color;
  app_->update();
}

void ChatUi::set_agent(const std::string& agent) {
  agent_label_->set_text(cpptui::StyledText(agent));
  app_->update();
}

void ChatUi::set_variant(const std::string& variant) {
  variant_label_->set_text(cpptui::StyledText(right_pad(variant, 24)));
  app_->update();
}

void ChatUi::set_context_usage(const std::string& usage) {
  context_label_->set_text(cpptui::StyledText(right_pad(usage, 24)));
  app_->update();
}

void ChatUi::set_error(const std::string& message) {
  error_label_->set_text(cpptui::StyledText(message));
  error_label_->visible = !message.empty();
  app_->update();
}

void ChatUi::clear_error() { set_error(""); }

void ChatUi::focus_prompt() { prompt_input_->set_focus(true); }

void ChatUi::submit_prompt() {
  std::string text = prompt_input_->get_text();
  if (text.empty()) return;
  prompt_input_->set_text("");
  emit(ActionType::SubmitPrompt, text);
  app_->update();
}

}
