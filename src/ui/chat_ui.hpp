#pragma once

#include <memory>
#include <string>
#include <vector>

#include "cpptui.hpp"
#include "ui/actions.hpp"
#include "ui/conversation_view.hpp"

namespace taia::ui {

class ChatUi {
 public:
  ChatUi(cpptui::App* app, ActionSink sink);

  std::shared_ptr<cpptui::Widget> root() const { return root_; }

  void append_message(const std::string& role, const std::string& text);
  void set_models(const std::vector<std::string>& ids,
                  const std::string& selected);
  void set_model_status(const std::string& status);
  void set_busy(bool busy);
  void set_error(const std::string& message);
  void clear_error();

  void submit_prompt();
  void focus_prompt();

 private:
  void emit(ActionType type, const std::string& text = "");
  std::string selected_model() const;

  cpptui::App* app_;
  ActionSink sink_;

  std::shared_ptr<cpptui::Vertical> root_;
  std::shared_ptr<ConversationView> conversation_;
  std::shared_ptr<cpptui::Dropdown> model_dropdown_;
  std::shared_ptr<cpptui::Spinner> spinner_;
  std::shared_ptr<cpptui::Label> status_label_;
  std::shared_ptr<cpptui::Label> error_label_;
  std::shared_ptr<cpptui::Input> prompt_input_;
};

}
