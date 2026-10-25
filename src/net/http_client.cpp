#include "net/http_client.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <utility>

namespace taia::net {

namespace {

struct ParsedUrl {
  std::string host;
  std::string port;
  std::string prefix;
};

bool parse_url(const std::string& url, ParsedUrl& out, std::string& error) {
  const std::string scheme = "http://";
  if (url.compare(0, scheme.size(), scheme) != 0) {
    error = "unsupported server URL (only http:// is supported): " + url;
    return false;
  }
  std::string rest = url.substr(scheme.size());
  std::string prefix;
  const auto slash = rest.find('/');
  if (slash != std::string::npos) {
    prefix = rest.substr(slash);
    rest = rest.substr(0, slash);
    if (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
  }
  std::string host;
  std::string port = "80";
  if (!rest.empty() && rest.front() == '[') {
    const auto close = rest.find(']');
    if (close == std::string::npos) {
      error = "invalid IPv6 host in URL: " + url;
      return false;
    }
    host = rest.substr(1, close - 1);
    if (close + 1 < rest.size()) {
      if (rest[close + 1] != ':') {
        error = "invalid server URL: " + url;
        return false;
      }
      port = rest.substr(close + 2);
    }
  } else {
    const auto colon = rest.rfind(':');
    if (colon != std::string::npos) {
      host = rest.substr(0, colon);
      port = rest.substr(colon + 1);
    } else {
      host = rest;
    }
  }
  if (host.empty()) {
    error = "missing host in server URL: " + url;
    return false;
  }
  if (port.empty()) port = "80";
  out = {host, port, prefix};
  return true;
}

std::string lower(std::string value) {
  for (char& c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

std::string trim(const std::string& value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return "";
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

class Connection {
 public:
  Connection() = default;
  ~Connection();
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  bool open(const ParsedUrl& url, std::string& error);
  bool write_all(const std::string& data, std::string& error);
  bool read_headers(int& status, std::string& error);

  enum class Pump { Data, End, Error };
  Pump pump(std::string& error);

  const std::string& decoded() const { return decoded_; }
  void consume(std::size_t count) { decoded_.erase(0, count); }

 private:
  bool fill_raw(std::string& error);
  bool read_raw_line(std::string& line, bool& eof, std::string& error);
  Pump pump_plain(std::string& error);
  Pump pump_chunked(std::string& error);

  int fd_ = -1;
  std::string raw_;
  std::string decoded_;
  bool eof_ = false;
  bool body_done_ = false;
  bool chunked_ = false;
  long long content_length_ = -1;
  long long content_read_ = 0;
  long long chunk_remaining_ = 0;
  bool chunk_need_terminator_ = false;
};

Connection::~Connection() {
  if (fd_ >= 0) ::close(fd_);
}

bool Connection::open(const ParsedUrl& url, std::string& error) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* result = nullptr;
  const int rc = ::getaddrinfo(url.host.c_str(), url.port.c_str(), &hints, &result);
  if (rc != 0) {
    error = "cannot resolve " + url.host + ": " + ::gai_strerror(rc);
    return false;
  }
  for (addrinfo* it = result; it != nullptr; it = it->ai_next) {
    const int fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
    if (fd < 0) continue;
    timeval timeout{};
    timeout.tv_sec = 30;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (::connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
      fd_ = fd;
      break;
    }
    ::close(fd);
  }
  ::freeaddrinfo(result);
  if (fd_ < 0) {
    error = "cannot connect to " + url.host + ":" + url.port;
    return false;
  }
  return true;
}

bool Connection::write_all(const std::string& data, std::string& error) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t n =
        ::send(fd_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    if (n > 0) {
      sent += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    error = std::string("send failed: ") + std::strerror(errno);
    return false;
  }
  return true;
}

bool Connection::read_headers(int& status, std::string& error) {
  std::string line;
  bool eof = false;
  if (!read_raw_line(line, eof, error)) {
    if (error.empty()) error = "connection closed before response";
    return false;
  }
  if (line.rfind("HTTP/", 0) != 0) {
    error = "invalid HTTP response: " + line;
    return false;
  }
  const auto first_space = line.find(' ');
  if (first_space == std::string::npos) {
    error = "invalid HTTP status line: " + line;
    return false;
  }
  const auto second_space = line.find(' ', first_space + 1);
  const std::string code =
      line.substr(first_space + 1, second_space == std::string::npos
                                      ? std::string::npos
                                      : second_space - first_space - 1);
  try {
    status = std::stoi(code);
  } catch (...) {
    error = "invalid HTTP status code: " + code;
    return false;
  }

  bool chunked = false;
  long long content_length = -1;
  while (true) {
    if (!read_raw_line(line, eof, error)) {
      if (error.empty()) error = "connection closed while reading headers";
      return false;
    }
    if (line.empty()) break;
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    const std::string name = lower(trim(line.substr(0, colon)));
    const std::string value = trim(line.substr(colon + 1));
    if (name == "transfer-encoding" &&
        lower(value).find("chunked") != std::string::npos) {
      chunked = true;
    } else if (name == "content-length") {
      try {
        content_length = std::stoll(value);
      } catch (...) {
        content_length = -1;
      }
    }
  }
  chunked_ = chunked;
  content_length_ = chunked ? -1 : content_length;
  return true;
}

Connection::Pump Connection::pump(std::string& error) {
  if (body_done_) return Pump::End;
  if (chunked_) return pump_chunked(error);
  return pump_plain(error);
}

bool Connection::fill_raw(std::string& error) {
  if (eof_) return false;
  char buffer[8192];
  while (true) {
    const ssize_t n = ::recv(fd_, buffer, sizeof(buffer), 0);
    if (n > 0) {
      raw_.append(buffer, static_cast<std::size_t>(n));
      return true;
    }
    if (n == 0) {
      eof_ = true;
      return false;
    }
    if (errno == EINTR) continue;
    eof_ = true;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      error = "timed out waiting for the server";
    } else {
      error = std::string("recv failed: ") + std::strerror(errno);
    }
    return false;
  }
}

bool Connection::read_raw_line(std::string& line, bool& eof,
                               std::string& error) {
  while (true) {
    auto pos = raw_.find("\r\n");
    if (pos != std::string::npos) {
      line = raw_.substr(0, pos);
      raw_.erase(0, pos + 2);
      eof = false;
      return true;
    }
    pos = raw_.find('\n');
    if (pos != std::string::npos) {
      line = raw_.substr(0, pos);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      raw_.erase(0, pos + 1);
      eof = false;
      return true;
    }
    if (!fill_raw(error)) {
      eof = true;
      if (!raw_.empty()) {
        line.swap(raw_);
        raw_.clear();
        return true;
      }
      return false;
    }
    if (raw_.size() > 64 * 1024) {
      error = "response line too long";
      eof = true;
      return false;
    }
  }
}

Connection::Pump Connection::pump_plain(std::string& error) {
  if (content_length_ >= 0) {
    if (content_read_ >= content_length_) {
      body_done_ = true;
      return Pump::End;
    }
    if (raw_.empty() && !fill_raw(error)) {
      if (!error.empty()) return Pump::Error;
      body_done_ = true;
      return Pump::End;
    }
    const std::size_t want = static_cast<std::size_t>(
        std::min<long long>(static_cast<long long>(raw_.size()),
                            content_length_ - content_read_));
    decoded_.append(raw_.data(), want);
    raw_.erase(0, want);
    content_read_ += static_cast<long long>(want);
    if (content_read_ >= content_length_) body_done_ = true;
    return Pump::Data;
  }
  if (raw_.empty() && !fill_raw(error)) {
    if (!error.empty()) return Pump::Error;
    body_done_ = true;
    return Pump::End;
  }
  decoded_ += raw_;
  raw_.clear();
  return Pump::Data;
}

Connection::Pump Connection::pump_chunked(std::string& error) {
  while (true) {
    if (chunk_remaining_ == 0) {
      std::string line;
      bool eof = false;
      if (chunk_need_terminator_) {
        if (!read_raw_line(line, eof, error)) {
          if (!error.empty()) return Pump::Error;
          body_done_ = true;
          return Pump::End;
        }
        chunk_need_terminator_ = false;
        if (!line.empty()) {
          error = "malformed chunked encoding";
          return Pump::Error;
        }
      }
      if (!read_raw_line(line, eof, error)) {
        if (!error.empty()) return Pump::Error;
        body_done_ = true;
        return Pump::End;
      }
      if (line.empty()) continue;
      const auto semicolon = line.find(';');
      if (semicolon != std::string::npos) line = line.substr(0, semicolon);
      line = trim(line);
      unsigned long long size = 0;
      try {
        size = std::stoull(line, nullptr, 16);
      } catch (...) {
        error = "invalid chunk size: " + line;
        return Pump::Error;
      }
      if (size == 0) {
        while (true) {
          std::string trailer;
          bool trailer_eof = false;
          if (!read_raw_line(trailer, trailer_eof, error)) break;
          if (trailer.empty()) break;
        }
        body_done_ = true;
        return Pump::End;
      }
      chunk_remaining_ = static_cast<long long>(size);
    }
    if (raw_.empty() && !fill_raw(error)) {
      if (!error.empty()) return Pump::Error;
      body_done_ = true;
      return Pump::End;
    }
    const std::size_t take = static_cast<std::size_t>(std::min<long long>(
        static_cast<long long>(raw_.size()), chunk_remaining_));
    decoded_.append(raw_.data(), take);
    raw_.erase(0, take);
    chunk_remaining_ -= static_cast<long long>(take);
    if (chunk_remaining_ == 0) chunk_need_terminator_ = true;
    if (!decoded_.empty()) return Pump::Data;
  }
}

bool send_request(Connection& conn, const std::string& method,
                  const std::string& target, const std::string& host,
                  const std::string& body, const std::string& accept,
                  std::string& error) {
  std::string request = method + " " + target + " HTTP/1.1\r\n";
  request += "Host: " + host + "\r\n";
  request += "Accept: " + accept + "\r\n";
  request += "Connection: close\r\n";
  if (!body.empty()) {
    request += "Content-Type: application/json\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  }
  request += "\r\n";
  request += body;
  return conn.write_all(request, error);
}

}  // namespace

HttpClient::HttpClient(std::string base_url) : base_url_(std::move(base_url)) {}

HttpResponse HttpClient::request(const std::string& method,
                                 const std::string& path,
                                 const std::string& body) {
  HttpResponse response;
  ParsedUrl url;
  if (!parse_url(base_url_, url, response.error)) return response;

  Connection conn;
  if (!conn.open(url, response.error)) return response;
  if (!send_request(conn, method, url.prefix + path, url.host, body,
                    "application/json", response.error)) {
    return response;
  }
  if (!conn.read_headers(response.status, response.error)) return response;

  while (true) {
    const Connection::Pump pump = conn.pump(response.error);
    if (pump == Connection::Pump::Data) continue;
    if (pump == Connection::Pump::Error) return response;
    break;
  }
  response.body = conn.decoded();
  return response;
}

bool HttpClient::stream(const std::string& method, const std::string& path,
                        const std::string& body,
                        const std::function<bool(const SseEvent&)>& on_event,
                        std::string& error,
                        const std::atomic<bool>* cancel) {
  ParsedUrl url;
  if (!parse_url(base_url_, url, error)) return false;

  Connection conn;
  if (!conn.open(url, error)) return false;
  if (!send_request(conn, method, url.prefix + path, url.host, body,
                    "text/event-stream", error)) {
    return false;
  }

  int status = 0;
  if (!conn.read_headers(status, error)) return false;
  if (status < 200 || status >= 300) {
    while (true) {
      const Connection::Pump pump = conn.pump(error);
      if (pump == Connection::Pump::Data) continue;
      if (pump == Connection::Pump::Error) return false;
      break;
    }
    error = "HTTP " + std::to_string(status);
    if (!conn.decoded().empty()) error += ": " + conn.decoded();
    return false;
  }

  while (true) {
    if (cancel != nullptr && cancel->load()) {
      error = "cancelled";
      return false;
    }
    while (true) {
      const auto newline = conn.decoded().find('\n');
      if (newline == std::string::npos) break;
      std::string line = conn.decoded().substr(0, newline);
      conn.consume(newline + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.rfind("data:", 0) == 0) {
        std::string data = line.substr(5);
        if (!data.empty() && data.front() == ' ') data.erase(0, 1);
        if (!on_event(SseEvent{data})) return true;
      }
    }
    const Connection::Pump pump = conn.pump(error);
    if (pump == Connection::Pump::Data) continue;
    if (pump == Connection::Pump::Error) return false;
    break;
  }
  return true;
}

}
