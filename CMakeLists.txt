cmake_minimum_required(VERSION 3.16)
project(taia_ct LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

add_executable(taia_ct
  src/main.cpp
  src/app.cpp
  src/ui/chat_ui.cpp
  src/ui/conversation_view.cpp
  src/ui/prompt_input.cpp
)
target_include_directories(taia_ct PRIVATE src)
target_link_libraries(taia_ct PRIVATE cpptui::cpptui)
