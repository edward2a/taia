#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "client/llama_client.hpp"
#include "controller/session_controller.hpp"

namespace {

using taia::ControllerEvent;
using taia::SessionController;
using taia::client::LlamaClient;
using taia::client::ModelInfo;

int failures = 0;

void check(bool condition, const std::string& what) {
  if (!condition) {
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
  }
}

class FakeServer {
 public:
  explicit FakeServer(std::string response)
      : FakeServer(std::vector<std::string>{std::move(response)}) {}

  explicit FakeServer(std::vector<std::string> responses)
      : responses_(std::move(responses)) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    ::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    socklen_t length = sizeof(address);
    ::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &length);
    port_ = ntohs(address.sin_port);
    ::listen(fd_, 1);
    thread_ = std::thread([this] { serve(); });
  }

  ~FakeServer() {
    release();
    wait();
    if (fd_ >= 0) ::close(fd_);
  }

  void release() {
    released_ = true;
    if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);
  }

  void wait() {
    if (thread_.joinable()) thread_.join();
  }

  bool accepted() const { return accepted_.load(); }

  const std::string& request_headers() const { return request_; }

  std::string base_url() const {
    return "http://127.0.0.1:" + std::to_string(port_);
  }

 private:
  void serve() {
    for (const std::string& response : responses_) {
      const int client = ::accept(fd_, nullptr, nullptr);
      if (client < 0) return;
      read_request(client, request_);
      accepted_ = true;
      if (response.empty()) {
        while (!released_) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ::close(client);
        continue;
      }
      std::size_t sent = 0;
      while (sent < response.size()) {
        const ssize_t n = ::send(client, response.data() + sent,
                                 response.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) break;
        sent += static_cast<std::size_t>(n);
      }
      ::shutdown(client, SHUT_WR);
      char sink[1024];
      while (::recv(client, sink, sizeof(sink), 0) > 0) {
      }
      ::close(client);
    }
  }

  static void read_request(int client, std::string& data) {
    char buffer[4096];
    std::size_t header_end = std::string::npos;
    long long content_length = 0;
    while (true) {
      const ssize_t n = ::recv(client, buffer, sizeof(buffer), 0);
      if (n <= 0) return;
      data.append(buffer, static_cast<std::size_t>(n));
      if (header_end == std::string::npos) {
        header_end = data.find("\r\n\r\n");
        if (header_end != std::string::npos) {
          std::string headers = data.substr(0, header_end);
          std::string lower = headers;
          for (char& c : lower) c = std::tolower(static_cast<unsigned char>(c));
          const auto pos = lower.find("content-length:");
          if (pos != std::string::npos) {
            content_length = std::stoll(headers.substr(pos + 15));
          }
        }
      }
      if (header_end != std::string::npos &&
          data.size() >= header_end + 4 + static_cast<std::size_t>(content_length)) {
        return;
      }
    }
  }

  int fd_ = -1;
  int port_ = 0;
  std::vector<std::string> responses_;
  std::string request_;
  std::atomic<bool> released_{false};
  std::atomic<bool> accepted_{false};
  std::thread thread_;
};

std::string http_response(int status, const std::string& content_type,
                          const std::string& body) {
  return "HTTP/1.1 " + std::to_string(status) +
         " Status\r\nContent-Type: " + content_type +
         "\r\nContent-Length: " + std::to_string(body.size()) +
         "\r\nConnection: close\r\n\r\n" + body;
}

std::string chunked_response(const std::string& content_type,
                             const std::string& body) {
  std::string out = "HTTP/1.1 200 OK\r\nContent-Type: " + content_type +
                    "\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
  std::size_t pos = 0;
  while (pos < body.size()) {
    const std::size_t n = std::min<std::size_t>(37, body.size() - pos);
    std::ostringstream size;
    size << std::hex << n;
    out += size.str() + "\r\n" + body.substr(pos, n) + "\r\n";
    pos += n;
  }
  out += "0\r\n\r\n";
  return out;
}

std::string sse(const std::string& payload) {
  return "data: " + payload + "\n\n";
}

}

void test_list_models() {
  const std::string body =
      "{\"object\":\"list\",\"data\":["
      "{\"id\":\"mock/small\",\"status\":{\"value\":\"loaded\"}},"
      "{\"id\":\"mock/large\",\"status\":{\"value\":\"unloaded\","
      "\"failed\":true,\"exit_code\":2}}]}";
  FakeServer server(http_response(200, "application/json", body));
  LlamaClient llama(server.base_url());
  std::vector<ModelInfo> models;
  std::string error;
  check(llama.list_models(models, error), "list_models succeeds: " + error);
  check(models.size() == 2, "models size is 2");
  if (models.size() == 2) {
    check(models.at(0).id == "mock/small", "first model id");
    check(models.at(0).status == "loaded", "first model status");
    check(models.at(1).id == "mock/large", "second model id");
    check(models.at(1).failed, "second model failed flag");
    check(models.at(1).exit_code == 2, "second model exit code");
  }
}

void test_chunked_list_models() {
  const std::string body = "{\"object\":\"list\",\"data\":[]}";
  FakeServer server(chunked_response("application/json", body));
  LlamaClient llama(server.base_url());
  std::vector<ModelInfo> models;
  std::string error;
  check(llama.list_models(models, error),
        "chunked list_models succeeds: " + error);
  check(models.empty(), "chunked list is empty");
}

void test_load_error() {
  const std::string body =
      "{\"error\":{\"code\":400,\"message\":\"model is already running\","
      "\"type\":\"invalid_request_error\"}}";
  FakeServer server(http_response(400, "application/json", body));
  LlamaClient llama(server.base_url());
  std::string error;
  check(!llama.load_model("mock/small", error), "load_model fails");
  check(error == "model is already running", "load error message: " + error);
}

void test_stream_success() {
  std::string body;
  body += sse(
      "{\"choices\":[{\"finish_reason\":null,\"index\":0,"
      "\"delta\":{\"role\":\"assistant\",\"content\":null}}]}");
  body += sse(
      "{\"choices\":[{\"finish_reason\":null,\"index\":0,"
      "\"delta\":{\"content\":\"Hel\"}}]}");
  body += sse(
      "{\"choices\":[{\"finish_reason\":null,\"index\":0,"
      "\"delta\":{\"content\":\"lo\"}}]}");
  body += sse(
      "{\"choices\":[{\"finish_reason\":\"stop\",\"index\":0,"
      "\"delta\":{}}]}");
  body += sse(std::string("[") + "DONE" + "]");
  FakeServer server(chunked_response("text/event-stream", body));
  LlamaClient llama(server.base_url());
  std::string text;
  std::string error;
  const bool ok = llama.chat_stream(
      "mock/small", {{"user", "hi"}},
      [&](const std::string& delta) { text += delta; }, error);
  check(ok, "stream succeeds: " + error);
  check(text == "Hello", "stream text: " + text);
}

void test_stream_error() {
  std::string body;
  body += sse(
      "{\"choices\":[{\"finish_reason\":null,\"index\":0,"
      "\"delta\":{\"content\":\"Hi\"}}]}");
  body += sse(
      "{\"error\":{\"code\":500,\"message\":\"boom\","
      "\"type\":\"server_error\"}}");
  FakeServer server(chunked_response("text/event-stream", body));
  LlamaClient llama(server.base_url());
  std::string text;
  std::string error;
  const bool ok = llama.chat_stream(
      "mock/small", {{"user", "hi"}},
      [&](const std::string& delta) { text += delta; }, error);
  check(!ok, "mid-stream error fails");
  check(error == "boom", "mid-stream error message: " + error);
  check(text == "Hi", "partial stream text: " + text);
}

void test_stream_incomplete() {
  std::string body;
  body += sse(
      "{\"choices\":[{\"finish_reason\":null,\"index\":0,"
      "\"delta\":{\"content\":\"Hi\"}}]}");
  FakeServer server(chunked_response("text/event-stream", body));
  LlamaClient llama(server.base_url());
  std::string error;
  const bool ok = llama.chat_stream(
      "mock/small", {{"user", "hi"}}, [](const std::string&) {}, error);
  check(!ok, "incomplete stream fails");
  check(error.find("done sentinel") != std::string::npos,
        "incomplete stream message: " + error);
}

void test_invalid_models_json() {
  FakeServer server(http_response(200, "application/json", "{not json"));
  LlamaClient llama(server.base_url());
  std::vector<ModelInfo> models;
  std::string error;
  check(!llama.list_models(models, error), "invalid models json fails");
  check(error.find("invalid /models response") != std::string::npos,
        "invalid json message: " + error);
}

void test_bad_scheme() {
  LlamaClient llama("https://127.0.0.1:1");
  std::vector<ModelInfo> models;
  std::string error;
  check(!llama.list_models(models, error), "https URL rejected");
  check(error.find("http://") != std::string::npos,
        "scheme error message: " + error);
}

void test_truncated_content_length() {
  const std::string raw =
      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
      "Content-Length: 100\r\nConnection: close\r\n\r\n{\"object\":\"list\"}";
  FakeServer server(raw);
  LlamaClient llama(server.base_url());
  std::vector<ModelInfo> models;
  std::string error;
  check(!llama.list_models(models, error), "truncated body fails");
  check(error.find("truncated") != std::string::npos,
        "truncated body message: " + error);
}

void test_truncated_chunked() {
  const std::string raw =
      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
      "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "10\r\n{\"object\"";
  FakeServer server(raw);
  LlamaClient llama(server.base_url());
  std::vector<ModelInfo> models;
  std::string error;
  check(!llama.list_models(models, error), "truncated chunk fails");
  check(error.find("truncated") != std::string::npos,
        "truncated chunk message: " + error);
}

void test_stream_without_done() {
  std::string body;
  body += sse(
      "{\"choices\":[{\"finish_reason\":\"stop\",\"index\":0,"
      "\"delta\":{}}]}");
  FakeServer server(chunked_response("text/event-stream", body));
  LlamaClient llama(server.base_url());
  std::string error;
  const bool ok = llama.chat_stream(
      "mock/small", {{"user", "hi"}}, [](const std::string&) {}, error);
  check(!ok, "stream without done fails");
  check(error.find("done sentinel") != std::string::npos,
        "stream without done message: " + error);
}

void test_api_key() {
  FakeServer server(http_response(200, "application/json",
                                  "{\"object\":\"list\",\"data\":[]}"));
  LlamaClient llama(server.base_url(), "secret");
  std::vector<ModelInfo> models;
  std::string error;
  check(llama.list_models(models, error), "api key request succeeds: " + error);
  server.wait();
  check(server.request_headers().find("Authorization: Bearer secret") !=
            std::string::npos,
        "authorization header present");
}

void test_load_unexpected_body() {
  FakeServer server(http_response(200, "application/json", "{}"));
  LlamaClient llama(server.base_url());
  std::string error;
  check(!llama.load_model("mock/small", error), "load rejects missing success");
  check(error.find("unexpected load response") != std::string::npos,
        "unexpected load message: " + error);
}

void test_truncated_chunk_trailer() {
  const std::string raw =
      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
      "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "2\r\n{}\r\n0\r\n";
  FakeServer server(raw);
  LlamaClient llama(server.base_url());
  std::vector<ModelInfo> models;
  std::string error;
  check(!llama.list_models(models, error), "truncated trailer fails");
  check(error.find("truncated") != std::string::npos,
        "truncated trailer message: " + error);
}

void test_controller_unload_polling() {
  const std::string loaded =
      "{\"object\":\"list\",\"data\":[{\"id\":\"m\","
      "\"status\":{\"value\":\"loaded\"}}]}";
  const std::string unloaded =
      "{\"object\":\"list\",\"data\":[{\"id\":\"m\","
      "\"status\":{\"value\":\"unloaded\"}}]}";
  FakeServer server(
      {http_response(200, "application/json", "{\"success\":true}"),
       http_response(200, "application/json", loaded),
       http_response(200, "application/json", unloaded)});
  SessionController controller(server.base_url());
  controller.start();
  controller.set_selected_model("m");
  controller.unload_model();

  bool finished = false;
  bool errored = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline && !finished &&
         !errored) {
    ControllerEvent event;
    while (controller.poll_event(event)) {
      if (event.type == ControllerEvent::Type::UnloadFinished) finished = true;
      if (event.type == ControllerEvent::Type::Error) errored = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  controller.stop();
  check(finished, "controller unload finishes after polling");
  check(!errored, "controller unload reports no error");
}

void test_controller_shutdown_stall() {
  FakeServer server(std::vector<std::string>{std::string()});
  SessionController controller(server.base_url());
  controller.start();
  controller.set_selected_model("m");
  controller.submit_prompt("hi");
  const auto accept_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!server.accepted() &&
         std::chrono::steady_clock::now() < accept_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  check(server.accepted(), "server accepted the stalled request");

  const auto begin = std::chrono::steady_clock::now();
  controller.stop();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - begin);
  server.release();
  check(elapsed < std::chrono::milliseconds(2000),
        "shutdown cancels a stalled request");
}

int main() {
  test_list_models();
  test_chunked_list_models();
  test_load_error();
  test_stream_success();
  test_stream_error();
  test_stream_incomplete();
  test_invalid_models_json();
  test_bad_scheme();
  test_truncated_content_length();
  test_truncated_chunked();
  test_stream_without_done();
  test_api_key();
  test_load_unexpected_body();
  test_truncated_chunk_trailer();
  test_controller_unload_polling();
  test_controller_shutdown_stall();
  if (failures == 0) {
    std::cout << "all client tests passed\n";
    return 0;
  }
  std::cerr << failures << " client test(s) failed\n";
  return 1;
}
