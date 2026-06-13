cmake_minimum_required(VERSION 3.13)
project(hfdl_decoder)

file(GLOB_RECURSE SRC "src/*.cpp" "src/*.c")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(hfdl_decoder PRIVATE "src/")

find_package(Threads REQUIRED)
target_link_libraries(hfdl_decoder PRIVATE Threads::Threads)

if (MSVC)
    target_compile_options(hfdl_decoder PRIVATE /O2 /fp:fast)
else ()
    target_compile_options(hfdl_decoder PRIVATE -O3 -ffast-math)
endif ()
