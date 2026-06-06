cmake_minimum_required(VERSION 3.13)
project(dsd_decoder)

file(GLOB_RECURSE SRC "src/*.cpp" "src/*.c")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(dsd_decoder PRIVATE "src/")

# We spawn helper threads (writer / reader / LRRP tail) directly from the
# module, so make sure the pthread symbols are available to the .so itself.
find_package(Threads REQUIRED)
target_link_libraries(dsd_decoder PRIVATE Threads::Threads)
