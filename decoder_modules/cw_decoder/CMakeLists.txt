cmake_minimum_required(VERSION 3.13)
project(cw_decoder)

file(GLOB_RECURSE SRC "src/*.cpp" "src/*.c")
# Exclude the standalone test harness from the module build.
list(FILTER SRC EXCLUDE REGEX "src/test/.*")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(cw_decoder PRIVATE "src/")
