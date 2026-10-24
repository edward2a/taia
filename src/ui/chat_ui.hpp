#pragma once

#include <memory>
#include <string>
#include <vector>

#include "cpptui.hpp"
#include "ui/actions.hpp"
#include "ui/conversation_view.hpp"
#include "ui/prompt_input.hpp"

namespace taia::ui {

enum class RunState { Idle, Loading, Generating, Error };

class ChatUi {
 public:
  ChatUi(cpptui::App* app, ActionSink sink);

  std::shared_ptr<cpptui::Widget> root() const { return root_; }

  void append_message(const std::string& role, const std::string& text);
  void set_models(const std::vector<std::string>& ids,
                  const std::string& selected);
  void set_state(RunState state);
  void set_agent(const std::string& agent);
  void set_variant(const std::string& variant);
  void set_context_usage(const std::string& usage);
  void set_error(const std::string& message);
  void clear_error();

  void submit_prompt();
  void focus_prompt();

 private:
  void emit(ActionType type, const std::string& text = "");
  std::string selected_model() const;
  void update_model_width();

  cpptui::App* app_;
  ActionSink sink_;

  std::shared_ptr<cpptui::Vertical> root_;
  std::shared_ptr<ConversationView> conversation_;
  std::shared_ptr<cpptui::Dropdown> model_dropdown_;
  std::shared_ptr<cpptui::Label> state_label_;
  std::shared_ptr<cpptui::Label> agent_label_;
  std::shared_ptr<cpptui::Label> variant_label_;
  std::shared_ptr<cpptui::Label> context_label_;
  std::shared_ptr<cpptui::Label> error_label_;
  std::shared_ptr<PromptInput> prompt_input_;
};

}
