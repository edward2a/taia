#include "ui/chat_ui.hpp"

#include <utility>

namespace taia::ui {

ChatUi::ChatUi(cpptui::App* app, ActionSink sink)
    : app_(app), sink_(std::move(sink)) {
  root_ = std::make_shared<cpptui::Vertical>();

  auto header = std::make_shared<cpptui::Horizontal>();
  header->fixed_height = 1;
  header->add(std::make_shared<cpptui::Label>(cpptui::StyledText("taia-ct")));
  header->add(std::make_shared<cpptui::HorizontalSpacer>(2));
  header->add(std::make_shared<cpptui::Label>(cpptui::StyledText("model:")));
  model_dropdown_ = std::make_shared<cpptui::Dropdown>(app_);
  model_dropdown_->placeholder = "no model";
  model_dropdown_->min_width = 32;
  model_dropdown_->on_change = [this](int, const std::string& value) {
    emit(ActionType::SelectModel, value);
  };
  header->add(model_dropdown_);
  header->add(std::make_shared<cpptui::HorizontalSpacer>(1));
  spinner_ = std::make_shared<cpptui::Spinner>(app_);
  spinner_->visible = false;
  header->add(spinner_);
  header->add(std::make_shared<cpptui::HorizontalSpacer>(1));
  status_label_ = std::make_shared<cpptui::Label>(cpptui::StyledText("idle"));
  header->add(status_label_);
  root_->add(header);

  auto border = std::make_shared<cpptui::Border>(cpptui::BorderStyle::Rounded);
  border->set_title("Conversation");
  conversation_ = std::make_shared<ConversationView>();
  border->add(conversation_);
  root_->add(border);

  error_label_ = std::make_shared<cpptui::Label>(cpptui::StyledText(""));
  error_label_->fixed_height = 1;
  error_label_->fg_color = cpptui::Theme::current().error;
  error_label_->visible = false;
  root_->add(error_label_);

  auto prompt_row = std::make_shared<cpptui::Horizontal>();
  prompt_row->fixed_height = 1;
  prompt_row->add(std::make_shared<cpptui::Label>(cpptui::StyledText("> ")));
  prompt_input_ = std::make_shared<cpptui::Input>();
  prompt_input_->placeholder = "Type a message and press Enter";
  prompt_row->add(prompt_input_);
  root_->add(prompt_row);

  app_->register_key(
      13, [this] {
        if (prompt_input_->has_focus()) submit_prompt();
      },
      false, false, false, false);
  app_->register_key(
      10, [this] {
        if (prompt_input_->has_focus()) submit_prompt();
      },
      false, false, false, false);
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
  app_->update();
}

void ChatUi::set_model_status(const std::string& status) {
  status_label_->set_text(cpptui::StyledText(status));
  app_->update();
}

void ChatUi::set_busy(bool busy) {
  spinner_->visible = busy;
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
  std::string text = prompt_input_->get_value();
  if (text.empty()) return;
  prompt_input_->set_text("");
  emit(ActionType::SubmitPrompt, text);
  app_->update();
}

}
