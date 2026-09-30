cmake_minimum_required(VERSION 3.20)
project(BetaGo LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(betago src/state.cpp src/random.cpp src/mcts.cpp src/runner.cpp)
target_include_directories(betago PUBLIC include third_party)
add_executable(runner src/runner_main.cpp)
target_link_libraries(runner PRIVATE betago)
add_executable(tests tests/test_main.cpp tests/test_mcts.cpp)
target_link_libraries(tests PRIVATE betago)
enable_testing()
add_test(NAME rules_runner_and_mcts COMMAND tests --oracle ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/migration.json)

if(WIN32)
    add_executable(play src/play_main.cpp src/window.cpp src/tk.cpp)
    target_include_directories(play PRIVATE third_party/tcl)
    target_link_libraries(play PRIVATE betago)
    add_test(NAME visual_board COMMAND play --self-test)
    # GUI executables need the bundled Tk runtime beside them. build.ps1 sets
    # it up; copy build/runtime beside play.exe when using a CMake build tree.
endif()
