#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <signal.h>
#include <string>
#include <termios.h>
#include <unistd.h>
#include <vector>

#include "app.hpp"
#include "cli_options.hpp"

namespace {
namespace cli = taia::cli;

termios g_prompt_termios{};
volatile sig_atomic_t g_prompt_signal_guard = 0;
struct sigaction g_prompt_previous_actions[4]{};
constexpr int kPromptSignals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT};
constexpr std::size_t kPromptSignalCount = 4;

void prompt_signal_handler(int signal_number) {
  if (g_prompt_signal_guard) {
    ::tcsetattr(STDIN_FILENO, TCSANOW, &g_prompt_termios);
  }
  ::signal(signal_number, SIG_DFL);
  ::raise(signal_number);
}

void restore_prompt_signal_handlers() {
  for (std::size_t i = 0; i < kPromptSignalCount; ++i) {
    ::sigaction(kPromptSignals[i], &g_prompt_previous_actions[i], nullptr);
  }
}

std::optional<std::string> environment_value(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr) return std::nullopt;
  return std::string(value);
}

class HiddenEcho {
 public:
  ~HiddenEcho() { restore(); }

  void restore() {
    if (!active_) return;
    ::tcsetattr(STDIN_FILENO, TCSANOW, &original_);
    g_prompt_signal_guard = 0;
    restore_prompt_signal_handlers();
    active_ = false;
  }

  bool enable(std::string& error) {
    if (::tcgetattr(STDIN_FILENO, &original_) != 0) {
      error = "cannot read terminal settings";
      return false;
    }

    g_prompt_termios = original_;
    struct sigaction action{};
    action.sa_handler = prompt_signal_handler;
    ::sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    std::size_t installed = 0;
    for (; installed < kPromptSignalCount; ++installed) {
      if (::sigaction(kPromptSignals[installed], &action,
                      &g_prompt_previous_actions[installed]) != 0) {
        for (std::size_t i = 0; i < installed; ++i) {
          ::sigaction(kPromptSignals[i], &g_prompt_previous_actions[i],
                      nullptr);
        }
        error = "cannot install terminal signal handlers";
        return false;
      }
    }
    g_prompt_signal_guard = 1;

    termios hidden = original_;
    hidden.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON | ISIG);
    hidden.c_cc[VMIN] = 1;
    hidden.c_cc[VTIME] = 0;
    if (::tcsetattr(STDIN_FILENO, TCSANOW, &hidden) != 0) {
      g_prompt_signal_guard = 0;
      restore_prompt_signal_handlers();
      error = "cannot prepare terminal input";
      return false;
    }
    active_ = true;
    return true;
  }

 private:
  termios original_{};
  bool active_ = false;
};

std::optional<std::string> prompt_api_key(std::string& error) {
  if (!::isatty(STDIN_FILENO)) {
    error = "--prompt-api-key requires a terminal on stdin";
    return std::nullopt;
  }

  HiddenEcho hidden_echo;
  if (!hidden_echo.enable(error)) return std::nullopt;

  std::cout << "API key: " << std::flush;
  std::string key;
  bool eof = false;
  bool too_long = false;
  while (true) {
    char ch = 0;
    const ssize_t count = ::read(STDIN_FILENO, &ch, 1);
    if (count == 0) {
      eof = true;
      break;
    }
    if (count < 0) {
      if (errno == EINTR) continue;
      error = "cannot read API key";
      break;
    }
    if (ch == '\x03' || ch == '\x1a' || ch == '\x1c') {
      error = "API key entry cancelled";
      break;
    }
    if (ch == '\x04') {
      eof = true;
      break;
    }
    if (ch == '\n' || ch == '\r') break;
    if (ch == '\x7f' || ch == '\x08') {
      if (!key.empty()) key.pop_back();
      continue;
    }
    if (ch == '\x15') {
      key.clear();
      continue;
    }
    if (key.size() >= 4096) {
      too_long = true;
      continue;
    }
    key.push_back(ch);
  }
  std::cout << "\n" << std::flush;

  if (!error.empty()) return std::nullopt;
  if (eof) {
    error = "no API key provided";
    return std::nullopt;
  }
  if (too_long) {
    error = "API key is too long";
    return std::nullopt;
  }
  if (key.empty()) {
    error = "API key cannot be empty";
    return std::nullopt;
  }
  return key;
}

}

int main(int argc, char** argv) {
  const std::string program =
      argc > 0 && argv[0] != nullptr
          ? std::filesystem::path(argv[0]).filename().string()
          : "taia_ct";

  std::vector<std::string> args;
  args.reserve(argc > 0 ? static_cast<std::size_t>(argc - 1) : 0);
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

  const cli::ParseResult parsed = cli::parse(args);
  if (!parsed.ok()) {
    std::cerr << "error: " << parsed.error << "\n\n" << cli::usage(program);
    return 2;
  }
  if (parsed.options.help) {
    std::cout << cli::usage(program);
    return 0;
  }

  const cli::Environment environment{
      environment_value("TAIA_SERVER_URL"),
      environment_value("TAIA_WORKSPACE"),
      environment_value("TAIA_STREAM_START_TIMEOUT_SECONDS"),
      environment_value("TAIA_STREAM_IDLE_TIMEOUT_SECONDS"),
  };

  std::error_code ec;
  const std::string current_directory =
      std::filesystem::current_path(ec).string();
  const cli::Config config =
      cli::resolve_config(parsed.options, environment, current_directory);

  std::string api_key;
  if (const auto value = environment_value("TAIA_API_KEY")) api_key = *value;
  if (config.prompt_api_key) {
    std::string error;
    const auto prompted = prompt_api_key(error);
    if (!prompted) {
      std::cerr << "error: " << error << "\n";
      return 1;
    }
    api_key = *prompted;
  }

  taia::net::StreamTimeouts timeouts;
  timeouts.start = std::chrono::seconds(config.stream_start_timeout_seconds);
  timeouts.idle = std::chrono::seconds(config.stream_idle_timeout_seconds);

  taia::App app;
  return app.run(config.server_url, api_key, timeouts, config.workspace);
}
