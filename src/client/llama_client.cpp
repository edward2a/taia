#include "client/llama_client.hpp"

#include <exception>
#include <utility>

#include <nlohmann/json.hpp>

namespace taia::client {

namespace {

using json = nlohmann::json;

std::string error_message_from_json(const json& value) {
  if (value.is_string()) return value.get<std::string>();
  if (value.is_object() && value.contains("message") &&
      value["message"].is_string()) {
    return value["message"].get<std::string>();
  }
  return value.dump();
}

std::string error_message(const std::string& body) {
  if (body.empty()) return "request failed";
  try {
    const json parsed = json::parse(body);
    if (parsed.is_object() && parsed.contains("error")) {
      return error_message_from_json(parsed["error"]);
    }
  } catch (const std::exception&) {
  }
  return body;
}

bool success_response(const std::string& body, const std::string& action,
                      std::string& error) {
  try {
    const json parsed = json::parse(body);
    if (parsed.is_object() && parsed.contains("success") &&
        parsed["success"].is_boolean() && parsed["success"].get<bool>()) {
      return true;
    }
  } catch (const std::exception& exc) {
    error = "invalid " + action + " response: " + exc.what();
    return false;
  }
  error = "unexpected " + action + " response";
  return false;
}

}  // namespace

LlamaClient::LlamaClient(std::string base_url, std::string api_key,
                         net::StreamTimeouts timeouts)
    : http_(std::move(base_url), std::move(api_key), timeouts) {}

bool LlamaClient::list_models(std::vector<ModelInfo>& models,
                              std::string& error,
                              const std::atomic<bool>* cancel) {
  const net::HttpResponse response = http_.request("GET", "/models", "", cancel);
  if (!response.error.empty()) {
    error = response.error;
    return false;
  }
  if (response.status != 200) {
    error = error_message(response.body);
    return false;
  }
  try {
    const json parsed = json::parse(response.body);
    if (!parsed.is_object() || !parsed.contains("data") ||
        !parsed["data"].is_array()) {
      error = "invalid /models response";
      return false;
    }
    models.clear();
    for (const json& item : parsed["data"]) {
      if (!item.is_object()) continue;
      ModelInfo model;
      if (item.contains("id") && item["id"].is_string()) {
        model.id = item["id"].get<std::string>();
      }
      if (item.contains("status") && item["status"].is_object()) {
        const json& status = item["status"];
        if (status.contains("value") && status["value"].is_string()) {
          model.status = status["value"].get<std::string>();
        }
        if (status.contains("failed") && status["failed"].is_boolean()) {
          model.failed = status["failed"].get<bool>();
        }
        if (status.contains("exit_code") &&
            status["exit_code"].is_number_integer()) {
          model.exit_code = status["exit_code"].get<int>();
        }
      }
      if (!model.id.empty()) models.push_back(std::move(model));
    }
  } catch (const std::exception& exc) {
    error = std::string("invalid /models response: ") + exc.what();
    return false;
  }
  return true;
}

bool LlamaClient::load_model(const std::string& model, std::string& error,
                             const std::atomic<bool>* cancel) {
  const json request{{"model", model}};
  const net::HttpResponse response =
      http_.request("POST", "/models/load", request.dump(), cancel);
  if (!response.error.empty()) {
    error = response.error;
    return false;
  }
  if (response.status != 200) {
    error = error_message(response.body);
    return false;
  }
  return success_response(response.body, "load", error);
}

bool LlamaClient::unload_model(const std::string& model, std::string& error,
                               const std::atomic<bool>* cancel) {
  const json request{{"model", model}};
  const net::HttpResponse response =
      http_.request("POST", "/models/unload", request.dump(), cancel);
  if (!response.error.empty()) {
    error = response.error;
    return false;
  }
  if (response.status != 200) {
    error = error_message(response.body);
    return false;
  }
  return success_response(response.body, "unload", error);
}

bool LlamaClient::chat_turn(
    const std::string& model, const std::vector<ChatMessage>& messages,
    const std::vector<ToolDefinition>& tools,
    const std::function<void(const std::string&)>& on_delta, ChatResult& result,
    std::string& error, const std::atomic<bool>* cancel) {
  result = ChatResult{};
  json request;
  request["model"] = model;
  request["stream"] = true;
  request["stream_options"] = json{{"include_usage", true}};
  request["messages"] = json::array();
  for (const ChatMessage& message : messages) {
    json item{{"role", message.role}, {"content", message.content}};
    if (!message.tool_calls.empty()) {
      item["tool_calls"] = json::array();
      for (const ToolCall& call : message.tool_calls) {
        item["tool_calls"].push_back(
            json{{"id", call.id},
                 {"type", "function"},
                 {"function",
                  {{"name", call.name}, {"arguments", call.arguments}}}});
      }
    }
    if (!message.tool_call_id.empty()) {
      item["tool_call_id"] = message.tool_call_id;
    }
    request["messages"].push_back(std::move(item));
  }
  if (!tools.empty()) {
    request["tools"] = json::array();
    for (const ToolDefinition& tool : tools) {
      json parameters;
      try {
        parameters = json::parse(tool.parameters);
      } catch (const std::exception& exc) {
        error = "invalid schema for tool " + tool.name + ": " + exc.what();
        return false;
      }
      request["tools"].push_back(
          json{{"type", "function"},
               {"function",
                {{"name", tool.name},
                 {"description", tool.description},
                 {"parameters", std::move(parameters)}}}});
    }
  }

  std::string done_marker;
  done_marker += static_cast<char>(91);
  done_marker += "DONE";
  done_marker += static_cast<char>(93);
  bool saw_done = false;
  bool failed = false;
  bool saw_output = false;
  std::vector<ToolCall> tool_calls;
  const bool transport_ok = http_.stream(
      "POST", "/v1/chat/completions", request.dump(),
      [&](const net::SseEvent& event) -> bool {
        if (event.data == done_marker) {
          saw_done = true;
          return false;
        }
        try {
          const json parsed = json::parse(event.data);
          if (parsed.is_object() && parsed.contains("error")) {
            error = error_message_from_json(parsed["error"]);
            failed = true;
            return false;
          }
          if (!parsed.is_object() || !parsed.contains("choices") ||
              !parsed["choices"].is_array() || parsed["choices"].empty()) {
            return true;
          }
          const json& choice = parsed["choices"][0];
          if (!choice.is_object()) return true;
          if (choice.contains("finish_reason") &&
              choice["finish_reason"].is_string()) {
            result.finish_reason = choice["finish_reason"].get<std::string>();
          }
          if (!choice.contains("delta") || !choice["delta"].is_object()) {
            return true;
          }
          const json& delta = choice["delta"];
          if (delta.contains("content") && delta["content"].is_string()) {
            const std::string text = delta["content"].get<std::string>();
            if (!text.empty()) {
              saw_output = true;
              result.content += text;
              if (on_delta) on_delta(text);
            }
          }
          if (!delta.contains("tool_calls") || !delta["tool_calls"].is_array()) {
            return true;
          }
          for (const json& raw : delta["tool_calls"]) {
            if (!raw.is_object()) continue;
            int index = 0;
            if (raw.contains("index") && raw["index"].is_number_integer()) {
              index = raw["index"].get<int>();
            }
            constexpr int kMaxToolCalls = 16;
            if (index < 0 || index >= kMaxToolCalls) {
              error = "invalid tool call index";
              failed = true;
              return false;
            }
            if (static_cast<std::size_t>(index) >= tool_calls.size()) {
              tool_calls.resize(static_cast<std::size_t>(index) + 1);
            }
            ToolCall& call = tool_calls[static_cast<std::size_t>(index)];
            if (raw.contains("id") && raw["id"].is_string()) {
              call.id = raw["id"].get<std::string>();
            }
            if (!raw.contains("function") || !raw["function"].is_object()) {
              continue;
            }
            const json& function = raw["function"];
            bool progressed = false;
            if (function.contains("name") && function["name"].is_string()) {
              call.name = function["name"].get<std::string>();
              progressed = true;
            }
            if (function.contains("arguments") &&
                function["arguments"].is_string()) {
              call.arguments += function["arguments"].get<std::string>();
              progressed = true;
            }
            if (progressed) saw_output = true;
          }
        } catch (const std::exception& exc) {
          error = std::string("invalid stream event: ") + exc.what();
          failed = true;
          return false;
        }
        return true;
      },
      [&] { return saw_output; }, error, cancel);
  if (!transport_ok) return false;
  if (failed) return false;
  if (!saw_done) {
    error = "stream ended without done sentinel";
    return false;
  }
  for (std::size_t i = 0; i < tool_calls.size(); ++i) {
    ToolCall& call = tool_calls[i];
    if (call.name.empty()) {
      error = "tool call is missing a name";
      return false;
    }
    if (call.id.empty()) {
      call.id = "call_" + std::to_string(i);
    }
    if (call.arguments.empty()) call.arguments = "{}";
    if (!json::accept(call.arguments)) {
      error = "invalid arguments for tool " + call.name;
      return false;
    }
    result.tool_calls.push_back(std::move(call));
  }
  return true;
}

bool LlamaClient::chat_stream(
    const std::string& model, const std::vector<ChatMessage>& messages,
    const std::function<void(const std::string&)>& on_delta, std::string& error,
    const std::atomic<bool>* cancel) {
  ChatResult result;
  return chat_turn(model, messages, {}, on_delta, result, error, cancel);
}

}
