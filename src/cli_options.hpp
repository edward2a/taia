#pragma once

#include <optional>
#include <string>
#include <vector>

namespace taia::cli {

struct Options {
  bool help = false;
  bool prompt_api_key = false;
  std::optional<std::string> server_url;
  std::optional<std::string> workspace;
  std::optional<long> stream_start_timeout_seconds;
  std::optional<long> stream_idle_timeout_seconds;
};

struct ParseResult {
  Options options;
  std::string error;

  bool ok() const { return error.empty(); }
};

struct Environment {
  std::optional<std::string> server_url;
  std::optional<std::string> workspace;
  std::optional<std::string> stream_start_timeout_seconds;
  std::optional<std::string> stream_idle_timeout_seconds;
};

struct Config {
  std::string server_url;
  std::string workspace;
  long stream_start_timeout_seconds = 300;
  long stream_idle_timeout_seconds = 30;
  bool prompt_api_key = false;
};

ParseResult parse(const std::vector<std::string>& args);
Config resolve_config(const Options& options, const Environment& environment,
                      std::string current_directory);
std::string usage(const std::string& program);

}
