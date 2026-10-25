#include <cstdlib>
#include <string>

#include "app.hpp"

int main(int argc, char** argv) {
  std::string base_url = "http://127.0.0.1:9931";
  if (const char* env = std::getenv("TAIA_SERVER_URL")) {
    base_url = env;
  }
  if (argc > 1) base_url = argv[1];

  taia::App app;
  return app.run(base_url);
}
