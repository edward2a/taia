#pragma once

#include <functional>
#include <string>

namespace taia::ui {

enum class ActionType {
  SubmitPrompt,
  SelectModel,
  LoadModel,
  UnloadModel,
  RefreshModels,
  Approve,
  Reject,
  CancelStream,
};

struct Action {
  ActionType type;
  std::string text;
};

using ActionSink = std::function<void(const Action&)>;

}
