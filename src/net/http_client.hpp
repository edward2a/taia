#pragma once

#include <atomic>
#include <functional>
#include <string>

namespace taia::net {

struct HttpResponse {
  int status = 0;
  std::string body;
  std::string error;

  bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

struct SseEvent {
  std::string data;
};

class HttpClient {
 public:
  explicit HttpClient(std::string base_url, std::string api_key = {});

  const std::string& base_url() const { return base_url_; }

  HttpResponse request(const std::string& method, const std::string& path,
                       const std::string& body,
                       const std::atomic<bool>* cancel = nullptr);

  bool stream(const std::string& method, const std::string& path,
              const std::string& body,
              const std::function<bool(const SseEvent&)>& on_event,
              std::string& error,
              const std::atomic<bool>* cancel = nullptr);

 private:
  std::string base_url_;
  std::string api_key_;
};

}
