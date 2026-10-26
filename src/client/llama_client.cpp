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

LlamaClient::LlamaClient(std::string base_url, std::string api_key)
    : http_(std::move(base_url), std::move(api_key)) {}

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

bool LlamaClient::chat_stream(
    const std::string& model, const std::vector<ChatMessage>& messages,
    const std::function<void(const std::string&)>& on_delta, std::string& error,
    const std::atomic<bool>* cancel) {
  json request;
  request["model"] = model;
  request["stream"] = true;
  request["stream_options"] = json{{"include_usage", true}};
  request["messages"] = json::array();
  for (const ChatMessage& message : messages) {
    request["messages"].push_back(
        json{{"role", message.role}, {"content", message.content}});
  }

  std::string done_marker;
  done_marker += static_cast<char>(91);
  done_marker += "DONE";
  done_marker += static_cast<char>(93);
  bool saw_done = false;
  bool failed = false;
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
          if (choice.contains("delta") && choice["delta"].is_object()) {
            const json& delta = choice["delta"];
            if (delta.contains("content") && delta["content"].is_string()) {
              const std::string text = delta["content"].get<std::string>();
              if (!text.empty() && on_delta) on_delta(text);
            }
          }
        } catch (const std::exception& exc) {
          error = std::string("invalid stream event: ") + exc.what();
          failed = true;
          return false;
        }
        return true;
      },
      error, cancel);
  if (!transport_ok) return false;
  if (failed) return false;
  if (!saw_done) {
    error = "stream ended without done sentinel";
    return false;
  }
  return true;
}

}
