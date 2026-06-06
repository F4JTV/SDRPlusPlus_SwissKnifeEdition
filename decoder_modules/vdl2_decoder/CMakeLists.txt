cmake_minimum_required(VERSION 3.13)
project(vdl2_decoder)

file(GLOB_RECURSE SRC "src/*.cpp" "src/*.c")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(vdl2_decoder PRIVATE "src/")

find_package(Threads REQUIRED)
target_link_libraries(vdl2_decoder PRIVATE Threads::Threads)

if (MSVC)
    target_compile_options(vdl2_decoder PRIVATE /O2 /fp:fast)
else ()
    target_compile_options(vdl2_decoder PRIVATE -O3 -ffast-math)
endif ()
