#include <iostream>
#include <string>
#include <vector>

#include "cli_options.hpp"

namespace {

using taia::cli::Config;
using taia::cli::Environment;
using taia::cli::Options;
using taia::cli::ParseResult;

int failures = 0;

void check(bool condition, const std::string& what) {
  if (!condition) {
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
  }
}

bool rejected(const std::vector<std::string>& args, const std::string& what) {
  const ParseResult result = taia::cli::parse(args);
  check(!result.ok(), what + " is rejected");
  check(!result.error.empty(), what + " has an error message");
  return !result.ok();
}

void test_flags() {
  const ParseResult result = taia::cli::parse(
      {"-u", "http://cli:8080", "-w", "/workspace",
       "--stream-start-timeout-seconds", "12",
       "--stream-idle-timeout-seconds", "34", "--prompt-api-key"});
  check(result.ok(), "all flags parse: " + result.error);
  check(result.options.server_url == "http://cli:8080", "server URL parsed");
  check(result.options.workspace == "/workspace", "workspace parsed");
  check(result.options.stream_start_timeout_seconds == 12,
        "start timeout parsed");
  check(result.options.stream_idle_timeout_seconds == 34,
        "idle timeout parsed");
  check(result.options.prompt_api_key, "API key prompt parsed");
  check(!result.options.help, "help not set");
}

void test_long_flags() {
  const ParseResult result =
      taia::cli::parse({"--server-url", "http://long:9000", "--workspace",
                        "/long", "--stream-start-timeout-seconds", "1",
                        "--stream-idle-timeout-seconds", "86400"});
  check(result.ok(), "long flags parse: " + result.error);
  check(result.options.server_url == "http://long:9000",
        "long server URL parsed");
  check(result.options.workspace == "/long", "long workspace parsed");
  check(result.options.stream_start_timeout_seconds == 1,
        "minimum timeout accepted");
  check(result.options.stream_idle_timeout_seconds == 86400,
        "maximum timeout accepted");
}

void test_help() {
  const ParseResult short_help = taia::cli::parse({"-h"});
  check(short_help.ok(), "short help parses");
  check(short_help.options.help, "short help sets help");

  const ParseResult help = taia::cli::parse({"--help", "--unknown"});
  check(help.ok(), "help ignores later arguments");
  check(help.options.help, "help sets help");

  const std::string text = taia::cli::usage("taia_ct");
  check(text.find("Usage: taia_ct") != std::string::npos,
        "usage names the program");
  check(text.find("--server-url") != std::string::npos, "usage lists URL");
  check(text.find("--workspace") != std::string::npos,
        "usage lists workspace");
  check(text.find("--stream-start-timeout-seconds") != std::string::npos,
        "usage lists start timeout");
  check(text.find("--stream-idle-timeout-seconds") != std::string::npos,
        "usage lists idle timeout");
  check(text.find("--prompt-api-key") != std::string::npos,
        "usage lists API key prompt");
  check(text.find("--api-key") == std::string::npos,
        "usage has no API key value option");
}

void test_positional_url() {
  const ParseResult result = taia::cli::parse({"http://positional:1234"});
  check(result.ok(), "positional URL parses");
  check(result.options.server_url == "http://positional:1234",
        "positional URL stored");
}

void test_conflicting_urls() {
  rejected({"--server-url", "http://a", "http://b"},
           "option URL plus positional URL");
  rejected({"http://b", "--server-url", "http://a"},
           "positional URL plus option URL");
}

void test_invalid_arguments() {
  rejected({"--api-key", "secret"}, "API key value option");
  rejected({"--unknown"}, "unknown long option");
  rejected({"-x"}, "unknown short option");
  rejected({"--server-url"}, "missing URL value");
  rejected({"-w"}, "missing workspace value");
  rejected({"--stream-start-timeout-seconds"}, "missing start timeout value");
  rejected({"--stream-idle-timeout-seconds"}, "missing idle timeout value");
  rejected({"--server-url", ""}, "empty URL value");
  rejected({"-w", ""}, "empty workspace value");
  rejected({"http://a", "http://b"}, "extra positional argument");
  rejected({""}, "empty positional argument");
  rejected({"---"}, "unknown dash option");
}

void test_timeout_bounds() {
  const ParseResult minimum =
      taia::cli::parse({"--stream-start-timeout-seconds", "1"});
  check(minimum.ok(), "timeout 1 accepted");

  const ParseResult maximum =
      taia::cli::parse({"--stream-idle-timeout-seconds", "86400"});
  check(maximum.ok(), "timeout 86400 accepted");

  rejected({"--stream-start-timeout-seconds", "0"}, "timeout 0");
  rejected({"--stream-start-timeout-seconds", "86401"}, "timeout 86401");
  rejected({"--stream-start-timeout-seconds", "-1"}, "negative timeout");
  rejected({"--stream-start-timeout-seconds", "1.5"}, "fractional timeout");
  rejected({"--stream-start-timeout-seconds", "12x"}, "suffixed timeout");
  rejected({"--stream-idle-timeout-seconds", "nope"}, "text timeout");
}

void test_defaults() {
  const Config config =
      taia::cli::resolve_config(Options{}, Environment{}, "/current");
  check(config.server_url == "http://127.0.0.1:9931",
        "default server URL");
  check(config.workspace == "/current", "default workspace");
  check(config.stream_start_timeout_seconds == 300,
        "default start timeout");
  check(config.stream_idle_timeout_seconds == 30, "default idle timeout");
  check(!config.prompt_api_key, "default does not prompt");
}

void test_environment() {
  Environment environment;
  environment.server_url = "http://env:7777";
  environment.workspace = "/env-workspace";
  environment.stream_start_timeout_seconds = "45";
  environment.stream_idle_timeout_seconds = "6";

  const Config config =
      taia::cli::resolve_config(Options{}, environment, "/current");
  check(config.server_url == "http://env:7777", "environment server URL");
  check(config.workspace == "/env-workspace", "environment workspace");
  check(config.stream_start_timeout_seconds == 45,
        "environment start timeout");
  check(config.stream_idle_timeout_seconds == 6,
        "environment idle timeout");
}

void test_cli_precedence() {
  Options options;
  options.server_url = "http://cli:8000";
  options.workspace = "/cli-workspace";
  options.stream_start_timeout_seconds = 20;
  options.stream_idle_timeout_seconds = 10;
  options.prompt_api_key = true;

  Environment environment;
  environment.server_url = "http://env:7777";
  environment.workspace = "/env-workspace";
  environment.stream_start_timeout_seconds = "45";
  environment.stream_idle_timeout_seconds = "6";

  const Config config =
      taia::cli::resolve_config(options, environment, "/current");
  check(config.server_url == "http://cli:8000", "CLI server URL wins");
  check(config.workspace == "/cli-workspace", "CLI workspace wins");
  check(config.stream_start_timeout_seconds == 20,
        "CLI start timeout wins");
  check(config.stream_idle_timeout_seconds == 10, "CLI idle timeout wins");
  check(config.prompt_api_key, "prompt flag resolves");
}

void test_invalid_environment_timeouts() {
  Environment environment;
  environment.stream_start_timeout_seconds = "nope";
  environment.stream_idle_timeout_seconds = "86401";

  const Config config =
      taia::cli::resolve_config(Options{}, environment, "/current");
  check(config.stream_start_timeout_seconds == 300,
        "invalid environment start timeout falls back");
  check(config.stream_idle_timeout_seconds == 30,
        "out-of-range environment idle timeout falls back");

  environment.stream_start_timeout_seconds = "30seconds";
  environment.stream_idle_timeout_seconds = " 30";
  const Config malformed =
      taia::cli::resolve_config(Options{}, environment, "/current");
  check(malformed.stream_start_timeout_seconds == 300,
        "suffixed environment timeout falls back");
  check(malformed.stream_idle_timeout_seconds == 30,
        "whitespace environment timeout falls back");
}

}

int main() {
  test_flags();
  test_long_flags();
  test_help();
  test_positional_url();
  test_conflicting_urls();
  test_invalid_arguments();
  test_timeout_bounds();
  test_defaults();
  test_environment();
  test_cli_precedence();
  test_invalid_environment_timeouts();
  if (failures == 0) {
    std::cout << "all CLI tests passed\n";
    return 0;
  }
  std::cerr << failures << " CLI test(s) failed\n";
  return 1;
}
