#include <chrono>
#include <cstdlib>
#include <string>

#include "app.hpp"

int main(int argc, char** argv) {
  std::string base_url = "http://127.0.0.1:9931";
  if (const char* env = std::getenv("TAIA_SERVER_URL")) {
    base_url = env;
  }
  if (argc > 1) base_url = argv[1];

  std::string api_key;
  if (const char* env = std::getenv("TAIA_API_KEY")) {
    api_key = env;
  }

  auto timeout_seconds = [](const char* name, long fallback) {
    const char* env = std::getenv(name);
    if (env == nullptr) return std::chrono::seconds(fallback);
    const long value = std::strtol(env, nullptr, 10);
    if (value < 1 || value > 86400) return std::chrono::seconds(fallback);
    return std::chrono::seconds(value);
  };
  taia::net::StreamTimeouts timeouts;
  timeouts.start = timeout_seconds("TAIA_STREAM_START_TIMEOUT_SECONDS", 300);
  timeouts.idle = timeout_seconds("TAIA_STREAM_IDLE_TIMEOUT_SECONDS", 30);

  taia::App app;
  return app.run(base_url, api_key, timeouts);
}
