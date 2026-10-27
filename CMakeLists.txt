cmake_minimum_required(VERSION 3.16)
project(taia_ct LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

include(FetchContent)

FetchContent_Declare(
  cpptui
  GIT_REPOSITORY https://github.com/jonoton/cpp-tui.git
  GIT_TAG        3ab1f3193c54b50adbaca019a448bfcff8daf159
)
FetchContent_MakeAvailable(cpptui)

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        55f93686c01528224f448c19128836e7df245f72
)
FetchContent_MakeAvailable(nlohmann_json)

add_executable(taia_ct
  src/main.cpp
  src/app.cpp
  src/cli_options.cpp
  src/client/llama_client.cpp
  src/controller/session_controller.cpp
  src/net/http_client.cpp
  src/tools/tools.cpp
  src/ui/approval_view.cpp
  src/ui/chat_ui.cpp
  src/ui/conversation_view.cpp
  src/ui/prompt_input.cpp
)
target_include_directories(taia_ct PRIVATE src)
target_link_libraries(taia_ct PRIVATE cpptui::cpptui nlohmann_json::nlohmann_json
                      Threads::Threads)

enable_testing()

add_executable(taia_client_test
  tests/cpp/test_client.cpp
  src/net/http_client.cpp
  src/client/llama_client.cpp
  src/controller/session_controller.cpp
  src/tools/tools.cpp
)
target_include_directories(taia_client_test PRIVATE src)
target_link_libraries(taia_client_test PRIVATE nlohmann_json::nlohmann_json
                      Threads::Threads)
add_test(NAME taia_client_test COMMAND taia_client_test)

add_executable(taia_tools_test
  tests/cpp/test_tools.cpp
  src/tools/tools.cpp
)
target_include_directories(taia_tools_test PRIVATE src)
target_link_libraries(taia_tools_test PRIVATE nlohmann_json::nlohmann_json
                      Threads::Threads)
add_test(NAME taia_tools_test COMMAND taia_tools_test)

add_executable(taia_cli_test
  tests/cpp/test_cli_options.cpp
  src/cli_options.cpp
)
target_include_directories(taia_cli_test PRIVATE src)
add_test(NAME taia_cli_test COMMAND taia_cli_test)
