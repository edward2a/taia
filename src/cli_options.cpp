#include "cli_options.hpp"

#include <charconv>
#include <system_error>
#include <utility>

namespace taia::cli {
namespace {

constexpr long kMinimumTimeout = 1;
constexpr long kMaximumTimeout = 86400;

ParseResult failure(Options options, std::string error) {
  return {std::move(options), std::move(error)};
}

bool parse_timeout(const std::string& text, long& value) {
  if (text.empty()) return false;
  long parsed = 0;
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end) return false;
  if (parsed < kMinimumTimeout || parsed > kMaximumTimeout) return false;
  value = parsed;
  return true;
}

long environment_timeout(const std::optional<std::string>& value,
                         long fallback) {
  if (!value) return fallback;
  long parsed = 0;
  if (!parse_timeout(*value, parsed)) return fallback;
  return parsed;
}

}

ParseResult parse(const std::vector<std::string>& args) {
  Options options;
  bool has_option_url = false;
  bool has_positional_url = false;

  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& arg = args[i];
    if (arg == "-h" || arg == "--help") {
      options.help = true;
      return {options, {}};
    }

    const auto next_value =
        [&](const std::string& option, std::string& value)
        -> std::optional<std::string> {
      if (i + 1 >= args.size()) return option + " requires a value";
      value = args[++i];
      if (value.empty()) return option + " requires a non-empty value";
      return std::nullopt;
    };

    if (arg == "-u" || arg == "--server-url") {
      std::string value;
      if (const auto error = next_value(arg, value)) {
        return failure(options, *error);
      }
      options.server_url = value;
      has_option_url = true;
      continue;
    }

    if (arg == "-w" || arg == "--workspace") {
      std::string value;
      if (const auto error = next_value(arg, value)) {
        return failure(options, *error);
      }
      options.workspace = value;
      continue;
    }

    if (arg == "--stream-start-timeout-seconds" ||
        arg == "--stream-idle-timeout-seconds") {
      std::string value;
      if (const auto error = next_value(arg, value)) {
        return failure(options, *error);
      }
      long seconds = 0;
      if (!parse_timeout(value, seconds)) {
        return failure(options, arg +
                                    " must be an integer between 1 and 86400");
      }
      if (arg == "--stream-start-timeout-seconds") {
        options.stream_start_timeout_seconds = seconds;
      } else {
        options.stream_idle_timeout_seconds = seconds;
      }
      continue;
    }

    if (arg == "--prompt-api-key") {
      options.prompt_api_key = true;
      continue;
    }

    if (!arg.empty() && arg.front() == '-') {
      return failure(options, "unknown option: " + arg);
    }

    if (arg.empty()) return failure(options, "unexpected empty argument");
    if (has_positional_url) {
      return failure(options, "unexpected argument: " + arg);
    }
    has_positional_url = true;
    options.server_url = arg;
  }

  if (has_option_url && has_positional_url) {
    return failure(options,
                   "server URL cannot be both positional and --server-url");
  }
  return {options, {}};
}

Config resolve_config(const Options& options, const Environment& environment,
                      std::string current_directory) {
  Config config;

  if (options.server_url) {
    config.server_url = *options.server_url;
  } else if (environment.server_url) {
    config.server_url = *environment.server_url;
  } else {
    config.server_url = "http://127.0.0.1:9931";
  }

  if (options.workspace) {
    config.workspace = *options.workspace;
  } else if (environment.workspace) {
    config.workspace = *environment.workspace;
  } else {
    config.workspace = std::move(current_directory);
  }

  config.stream_start_timeout_seconds =
      options.stream_start_timeout_seconds
          ? *options.stream_start_timeout_seconds
          : environment_timeout(environment.stream_start_timeout_seconds, 300);
  config.stream_idle_timeout_seconds =
      options.stream_idle_timeout_seconds
          ? *options.stream_idle_timeout_seconds
          : environment_timeout(environment.stream_idle_timeout_seconds, 30);
  config.prompt_api_key = options.prompt_api_key;
  return config;
}

std::string usage(const std::string& program) {
  return "Usage: " + program + " [OPTIONS] [SERVER_URL]\n"
         "\n"
         "Connect to a llama.cpp router server and run the coding agent.\n"
         "\n"
         "Options:\n"
         "  -h, --help                            Show this help and exit\n"
         "  -u, --server-url URL                  Server base URL\n"
         "  -w, --workspace PATH                  Workspace root\n"
         "      --stream-start-timeout-seconds N  First-token timeout\n"
         "      --stream-idle-timeout-seconds N   Inter-chunk idle timeout\n"
         "      --prompt-api-key                  Read the API key from hidden "
         "stdin\n"
         "\n"
         "Timeouts are whole seconds from 1 to 86400. Settings not given on\n"
         "the command line fall back to TAIA_* environment variables and then\n"
         "built-in defaults. The API key comes from TAIA_API_KEY unless\n"
         "--prompt-api-key is supplied.\n";
}

}
