cmake_minimum_required(VERSION 3.18)
project(quickstart LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(allsense
  GIT_REPOSITORY https://github.com/wen-da-ng/otterbridge-allsense-cpp.git
  GIT_TAG main)
FetchContent_MakeAvailable(allsense)

add_executable(quickstart quickstart.cpp)
target_link_libraries(quickstart PRIVATE allsense::allsense)
